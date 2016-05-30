/*
 * Copyright (c) 2015, Chaos Software Ltd
 *
 * V-Ray For Blender
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vfb_scene_exporter_pro.h"

#include <thread>
#include <chrono>

#include <Python.h>


void ProductionExporter::create_exporter()
{
	SceneExporter::create_exporter();

	if (m_exporter) {
		m_exporter->set_is_viewport(false);
		m_exporter->set_settings(m_settings);
	}
}


void ProductionExporter::setup_callbacks()
{
	m_exporter->set_callback_on_image_ready(ExpoterCallback(boost::bind(&ProductionExporter::cb_on_image_ready, this)));
	m_exporter->set_callback_on_rt_image_updated(ExpoterCallback(boost::bind(&ProductionExporter::cb_on_rt_image_updated, this)));
}

int	ProductionExporter::is_interrupted()
{
	bool is_interrupted = SceneExporter::is_interrupted();

	if (m_settings.settings_animation.use) {
		is_interrupted = is_interrupted || !m_isAnimationRunning;
	} else {
		is_interrupted = is_interrupted || m_renderFinished;
	}

	return is_interrupted;
}

bool ProductionExporter::export_animation_frame(const int &check_updated)
{
	using namespace std;
	using namespace std::chrono;

	bool frameExported = true;

	const bool onlyCamera = m_settings.settings_animation.use &&
		                    m_settings.settings_animation.mode == SettingsAnimation::AnimationModeCameraLoop;

	if (m_settings.exporter_type == ExpoterType::ExpoterTypeFile) {
		PRINT_INFO_EX("Exporting animation frame %d, in file", m_frameCurrent);
		sync(check_updated);
	} else {
		PRINT_INFO_EX("Exporting animation frame %d", m_frameCurrent);

		m_settings.settings_animation.frame_current = m_frameCurrent;
		m_exporter->set_current_frame(m_frameCurrent);

		m_exporter->stop();
		sync(check_updated);
		if (m_isFirstFrame) {
			render_start();
		}
		m_exporter->start();
		PRINT_INFO_EX("Waiting for renderer to render animation frame %f, current %f", m_frameCurrent, m_exporter->get_last_rendered_frame());

		auto lastTime = high_resolution_clock::now();
		while (m_exporter->get_last_rendered_frame() < m_frameCurrent) {
			this_thread::sleep_for(milliseconds(1));

			auto now = high_resolution_clock::now();
			if (duration_cast<seconds>(now - lastTime).count() > 1) {
				lastTime = now;
				PRINT_INFO_EX("Waiting for renderer to render animation frame %f, current %f", m_frameCurrent, m_exporter->get_last_rendered_frame());
			}
			if (this->is_interrupted()) {
				PRINT_INFO_EX("Interrupted - stopping animation rendering!");
				frameExported = false;
				break;
			}
			if (m_exporter->is_aborted()) {
				PRINT_INFO_EX("Renderer stopped - stopping animation rendering!");
				frameExported = false;
				break;
			}
		}
	}

	return frameExported;
}

bool ProductionExporter::do_export()
{
	bool res = true;
	PRINT_INFO_EX("ProductionExporter::do_export()");
	if (m_settings.exporter_type == ExpoterType::ExpoterTypeFile) {
		python_thread_state_restore();
	}

	if (m_settings.settings_animation.use) {
		m_isAnimationRunning = true;

		const bool is_camera_loop = m_settings.settings_animation.mode == SettingsAnimation::AnimationModeCameraLoop;

		m_frameCurrent = m_scene.frame_start();
		m_frameStep = m_scene.frame_step();
		m_frameCount = (m_scene.frame_end() - m_scene.frame_start()) / m_frameStep;

		const auto restore = m_scene.frame_current();
		m_animationProgress = 0.f;

		if (m_settings.exporter_type == ExpoterType::ExpoterTypeFile) {
			for (int c = 0; c < m_frameCount && res && !is_interrupted(); ++c) {
				m_animationProgress = (float)c / m_frameStep;
				m_frameCurrent = c * m_frameStep;
				m_isFirstFrame = c == 0;

				python_thread_state_restore();
					m_scene.frame_set(m_frameCurrent, 0.f);
					m_engine.update_progress(m_animationProgress);
				python_thread_state_save();

				PRINT_INFO_EX("Animation progress %d%%, frame %d", static_cast<int>(m_animationProgress * 100), m_frameCurrent);

				res = export_animation_frame(false);
			}
			m_scene.frame_set(restore, 0.f);
		} else {

			std::thread runner(&ProductionExporter::render_loop, this);
			std::vector<BL::Camera> loop_cameras;

			if (is_camera_loop) {
				BL::Scene::objects_iterator obIt;
				for (m_scene.objects.begin(obIt); obIt != m_scene.objects.end(); ++obIt) {
					BL::Object ob(*obIt);
					if (ob.type() == BL::Object::type_CAMERA) {
						loop_cameras.push_back(BL::Camera(ob));
					}
				}

				std::sort(loop_cameras.begin(), loop_cameras.end(), [](const BL::Camera & l, const BL::Camera & r) {
					return const_cast<BL::Camera&>(l).name() < const_cast<BL::Camera&>(r).name();
				});

				m_frameCount = loop_cameras.size();
				m_frameStep = 1;
				m_frameCurrent = 0;
			}
			m_exporter->stop();
			for (int c = 0; c < m_frameCount && res && !is_interrupted(); ++c) {
				if (is_camera_loop) {
					m_active_camera = loop_cameras[c];
				}
				m_isFirstFrame = c == 0;
				m_frameCurrent = m_frameStep * c;

				{
					std::lock_guard<std::mutex> l(m_python_state_lock);
					if (is_interrupted()) {
						break;
					}
					python_thread_state_restore();
						if (!is_camera_loop) {
							m_scene.frame_set(m_frameCurrent, 0.f);
						}
						m_engine.update_progress(m_animationProgress);
					python_thread_state_save();
				}

				res = export_animation_frame(false);
				while (res && !m_renderFinished && !is_interrupted()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}


			m_isAnimationRunning = false;
			m_renderFinished = true;
			runner.join();

			python_thread_state_restore();
			m_scene.frame_set(restore, 0.f);
			python_thread_state_save();
			render_end();

		}
	}
	else {
		sync(false);
	}

	if (m_settings.exporter_type == ExpoterType::ExpoterTypeFile) {
		python_thread_state_save();
	}

	return res;
}


void ProductionExporter::sync_dupli(BL::Object ob, const int &check_updated)
{
	ob.dupli_list_create(m_scene, EvalMode::EvalModeRender);

	SceneExporter::sync_dupli(ob, check_updated);

	ob.dupli_list_clear();
}

void ProductionExporter::sync_object_modiefiers(BL::Object ob, const int &check_updated)
{
	BL::Object::modifiers_iterator modIt;
	for (ob.modifiers.begin(modIt); modIt != ob.modifiers.end(); ++modIt) {
		BL::Modifier mod(*modIt);
		if (mod && mod.show_render() && mod.type() == BL::Modifier::type_PARTICLE_SYSTEM) {
			BL::ParticleSystemModifier psm(mod);
			BL::ParticleSystem psys = psm.particle_system();
			if (psys) {
				psys.set_resolution(m_scene, ob, EvalModeRender);
				m_data_exporter.exportHair(ob, psm, psys, check_updated);
				psys.set_resolution(m_scene, ob, EvalModePreview);
			}
		}
	}
}


void ProductionExporter::render_frame()
{
	if (!m_isRunning) {
		return;
	}
	m_imageDirty = true;

	std::unique_lock<std::mutex> uLock(m_python_state_lock, std::defer_lock_t());

	if (m_imageDirty) {
		m_imageDirty = false;
		float progress = 0.f;
		if (m_settings.settings_animation.use) {
			uLock.lock();
			if (is_interrupted()) {
				return;
			}
			python_thread_state_restore();

			// for animation add frames progress + current image progress * frame contribution
			float frame_contrib = 1.f / ((float)(m_scene.frame_end() - m_scene.frame_current()) / (float)m_scene.frame_step());
			progress = m_animationProgress + m_progress * frame_contrib;
		} else {
			// for singe frame - get progress from image
			progress = m_progress;
		}

		m_engine.update_progress(progress);
		for (auto & result : m_renderResultsList) {
			BL::RenderResult::layers_iterator rrlIt;
			result.layers.begin(rrlIt);
			if (rrlIt != result.layers.end()) {
				m_engine.update_result(result);
			}
		}

		if (m_settings.settings_animation.use) {
			python_thread_state_save();
			uLock.unlock();
		}
	}


	if (m_settings.settings_animation.use) {
		uLock.lock();
		if (is_interrupted()) {
			return;
		}
		python_thread_state_restore();
		// progress will be set from animation export loop
	} else {
		// single frame export - done
		m_engine.update_progress(1.f);
	}
	for (auto & result : m_renderResultsList) {
		BL::RenderResult::layers_iterator rrlIt;
		result.layers.begin(rrlIt);
		if (rrlIt != result.layers.end()) {
			m_engine.update_result(result);
		}
	}
	if (m_settings.settings_animation.use) {
		python_thread_state_save();
		uLock.unlock();
	}
}

void ProductionExporter::render_loop()
{
	while (!is_interrupted()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		render_frame();
	}
}

void ProductionExporter::render_start()
{
	BL::RenderSettings renderSettings = m_scene.render();

	BL::RenderSettings::layers_iterator rslIt;
	renderSettings.layers.begin(rslIt);
	if (rslIt != renderSettings.layers.end()) {
		BL::SceneRenderLayer sceneRenderLayer(*rslIt);
		if (sceneRenderLayer && !is_interrupted()) {
			BL::RenderResult renderResult = m_engine.begin_result(0, 0, m_viewParams.renderSize.w, m_viewParams.renderSize.h, sceneRenderLayer.name().c_str(), nullptr);
			if (renderResult) {
				m_renderResultsList.push_back(renderResult);
			}
		}
	}

	if (!is_preview()) {
		m_exporter->show_frame_buffer();
	}

	SceneExporter::render_start();

	m_isRunning = true;

	if (!m_settings.settings_animation.use) {
		render_loop();
		render_end();
	}
}

void ProductionExporter::render_end()
{
	if (m_settings.exporter_type != ExpoterType::ExpoterTypeFile) {
		std::lock_guard<std::mutex> l(m_callback_mtx);
		m_exporter->stop();
		m_exporter->set_callback_on_image_ready(ExpoterCallback());
		m_exporter->set_callback_on_rt_image_updated(ExpoterCallback());
	}
	python_thread_state_restore();
	for (auto & result : m_renderResultsList) {
		m_engine.end_result(result, false, true);
	}
	python_thread_state_save();
}

ProductionExporter::~ProductionExporter()
{
	{
		std::lock_guard<std::mutex> l(m_python_state_lock);
		if (m_settings.settings_animation.use) {
			m_isAnimationRunning = false;
		}
		if (m_python_thread_state) {
			python_thread_state_restore();
		}
	}
	{
		std::lock_guard<std::mutex> l(m_callback_mtx);
		delete m_exporter;
		m_exporter = nullptr;
	}
}


void ProductionExporter::cb_on_image_ready()
{
	std::lock_guard<std::mutex> l(m_callback_mtx);
	m_renderFinished = true;
}

void ProductionExporter::cb_on_rt_image_updated()
{
	std::lock_guard<std::mutex> l(m_callback_mtx);
	m_imageDirty = true;

	for (auto & result : m_renderResultsList) {
		BL::RenderResult::layers_iterator rrlIt;
		result.layers.begin(rrlIt);
		if (rrlIt != result.layers.end()) {
			BL::RenderLayer renderLayer(*rrlIt);
			if (renderLayer) {
				BL::RenderLayer::passes_iterator rpIt;
				for (renderLayer.passes.begin(rpIt); rpIt != renderLayer.passes.end(); ++rpIt) {
					BL::RenderPass renderPass(*rpIt);
					if (renderPass) {
						RenderImage image = m_exporter->get_pass(renderPass.type());

						if (image && image.w == m_viewParams.renderSize.w && image.h == m_viewParams.renderSize.h) {
							if (renderPass.type() == BL::RenderPass::type_COMBINED) {
								m_progress = image.updated;
							}
							renderPass.rect(image.pixels);
						}
					}
				}

				if (is_preview()) {
					python_thread_state_restore();
					m_engine.update_result(result);
					python_thread_state_save();
				}
			}
		}
	}
}
