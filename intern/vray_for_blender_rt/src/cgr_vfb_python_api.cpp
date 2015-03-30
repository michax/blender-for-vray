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

#include "cgr_config.h"

#include <Python.h>

#include <vraysdk.hpp>

#include "cgr_vray_for_blender_rt.h"
#include "cgr_scene_exporter.h"

#include "DNA_material_types.h"
#include "BLI_math.h"
#include "BKE_context.h"
#include "WM_api.h"
#include "WM_types.h"

extern "C" {
#  include "BKE_idprop.h"
#  include "mathutils/mathutils.h"
}

/* OpenGL header includes, used everywhere we use OpenGL, to deal with
 * platform differences in one central place. */

#ifdef WITH_GLEW_MX
#  include "glew-mx.h"
#else
#  include <GL/glew.h>
#  define mxCreateContext() glewInit()
#  define mxMakeCurrentContext(x) (x)
#endif


static VRay::VRayInit *VRayInit = nullptr;


static void *pylong_as_voidptr_typesafe(PyObject *object)
{
	if(object == Py_None)
		return NULL;
	return PyLong_AsVoidPtr(object);
}


static void python_thread_state_save(void **python_thread_state)
{
	*python_thread_state = (void*)PyEval_SaveThread();
}


static void python_thread_state_restore(void **python_thread_state)
{
	PyEval_RestoreThread((PyThreadState*)*python_thread_state);
	*python_thread_state = nullptr;
}


static PyObject* mExporterLoad(PyObject *self)
{
	PRINT_INFO_EX("mExporterLoad()");

	if (!VRayInit) {
		try {
			VRayInit = new VRay::VRayInit(false);
		}
		catch (...) {
			PRINT_INFO_EX("Error initing V-Ray");
			VRayInit = nullptr;
		}
	}

	Py_RETURN_NONE;
}


static PyObject* mExporterUnload(PyObject *self)
{
	PRINT_INFO_EX("mExporterUnload()");

	if (VRayInit) {
		delete VRayInit;
		VRayInit = nullptr;
	}

	Py_RETURN_NONE;
}


static PyObject* mExporterInit(PyObject *self, PyObject *args)
{
	PRINT_INFO_EX("mExporterInit()");

	PyObject *pyengine   = nullptr;
	PyObject *pyuserpref = nullptr;
	PyObject *pydata     = nullptr;
	PyObject *pyscene    = nullptr;
	PyObject *pyregion   = nullptr;
	PyObject *pyv3d      = nullptr;
	PyObject *pyrv3d     = nullptr;

	if(!PyArg_ParseTuple(args, "OOOOOOO", &pyengine, &pyuserpref, &pydata, &pyscene, &pyregion, &pyv3d, &pyrv3d)) {
		return NULL;
	}

	/* Create RNA pointers */
	PointerRNA engineptr;
	RNA_pointer_create(NULL, &RNA_RenderEngine, (void*)PyLong_AsVoidPtr(pyengine), &engineptr);
	BL::RenderEngine engine(engineptr);

	PointerRNA userprefptr;
	RNA_pointer_create(NULL, &RNA_UserPreferences, (void*)PyLong_AsVoidPtr(pyuserpref), &userprefptr);
	BL::UserPreferences userpref(userprefptr);

	PointerRNA dataptr;
	RNA_main_pointer_create((Main*)PyLong_AsVoidPtr(pydata), &dataptr);
	BL::BlendData data(dataptr);

	PointerRNA sceneptr;
	RNA_id_pointer_create((ID*)PyLong_AsVoidPtr(pyscene), &sceneptr);
	BL::Scene scene(sceneptr);

	PointerRNA regionptr;
	RNA_pointer_create(NULL, &RNA_Region, pylong_as_voidptr_typesafe(pyregion), &regionptr);
	BL::Region region(regionptr);

	PointerRNA v3dptr;
	RNA_pointer_create(NULL, &RNA_SpaceView3D, pylong_as_voidptr_typesafe(pyv3d), &v3dptr);
	BL::SpaceView3D v3d(v3dptr);

	PointerRNA rv3dptr;
	RNA_pointer_create(NULL, &RNA_RegionView3D, pylong_as_voidptr_typesafe(pyrv3d), &rv3dptr);
	BL::RegionView3D rv3d(rv3dptr);

	/* Create exporter */
	VRayForBlender::SceneExporter *exporter = new VRayForBlender::SceneExporter(engine, userpref, data, scene, v3d, rv3d, region);

	python_thread_state_save(&exporter->m_pythonThreadState);

	// exporter->init();

	python_thread_state_restore(&exporter->m_pythonThreadState);

	return PyLong_FromVoidPtr(exporter);
}


static PyObject* mExporterFree(PyObject *self, PyObject *value)
{
	PRINT_INFO_EX("mExporterFree()");

	delete (VRayForBlender::SceneExporter*)PyLong_AsVoidPtr(value);

	Py_RETURN_NONE;
}


static PyObject* mExporterExport(PyObject *self, PyObject *value)
{
	PRINT_INFO_EX("mExporterExport()");

	VRayForBlender::SceneExporter *exporter = (VRayForBlender::SceneExporter*)PyLong_AsVoidPtr(value);

	python_thread_state_save(&exporter->m_pythonThreadState);

	exporter->init();

	exporter->export_scene();
	exporter->render_start();

	python_thread_state_restore(&exporter->m_pythonThreadState);

	Py_RETURN_NONE;
}


static PyObject* mExporterUpdate(PyObject *self, PyObject *value)
{
	PRINT_INFO_EX("mExporterUpdate()");

	VRayForBlender::SceneExporter *exporter = (VRayForBlender::SceneExporter*)PyLong_AsVoidPtr(value);

	python_thread_state_save(&exporter->m_pythonThreadState);

	exporter->synchronize();

	python_thread_state_restore(&exporter->m_pythonThreadState);

	Py_RETURN_NONE;
}


static PyObject* mExporterDraw(PyObject *self, PyObject *args)
{
	PRINT_INFO_EX("mExporterDraw()");

	PyObject *pysession = nullptr;
	PyObject *pyv3d     = nullptr;
	PyObject *pyrv3d    = nullptr;

	if (!PyArg_ParseTuple(args, "OOO", &pysession, &pyv3d, &pyrv3d)) {
		return NULL;
	}

	if (pylong_as_voidptr_typesafe(pyrv3d)) {
		VRayForBlender::SceneExporter *exporter = (VRayForBlender::SceneExporter*)PyLong_AsVoidPtr(pysession);

		/* 3d view drawing */
		int viewport[4];
		glGetIntegerv(GL_VIEWPORT, viewport);

		exporter->draw(viewport[2], viewport[3]);
	}

	Py_RETURN_NONE;
}


static PyMethodDef methods[] = {
    {"load",   (PyCFunction)mExporterLoad,    METH_NOARGS,  ""},
    {"unload", (PyCFunction)mExporterUnload,  METH_NOARGS,  ""},

    {"init",    mExporterInit,    METH_VARARGS,  ""},
    {"free",    mExporterFree,    METH_O,        ""},

    {"export",  mExporterExport,  METH_O,        ""},
    {"update",  mExporterUpdate,  METH_O,        ""},
    {"draw",    mExporterDraw,    METH_VARARGS,  ""},

    {NULL, NULL, 0, NULL},
};


static struct PyModuleDef module = {
	PyModuleDef_HEAD_INIT,
	"_vray_for_blender_rt",
	"V-Ray For Blender Realtime Exporter",
	-1,
	methods,
	NULL, NULL, NULL, NULL
};


void* VRayForBlenderRT_initPython()
{
	return (void*)PyModule_Create(&module);
}
