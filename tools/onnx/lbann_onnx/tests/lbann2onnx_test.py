import re
import unittest
import os

import onnx
from onnx import numpy_helper
import numpy as np

import lbann_onnx.l2o
import lbann_onnx.util
import lbann_onnx.l2o.util
from lbann_onnx import getLbannRoot
from lbann_onnx.util import parseBoolEnvVar
from lbann_onnx.tests.util import isModelDumpEnabled, createAndGetDumpedModelsDir

LBANN_MODEL_ROOT = "{}/model_zoo/models".format(getLbannRoot())
SAVE_ONNX = isModelDumpEnabled()
DUMP_DIR = createAndGetDumpedModelsDir()
ADD_DUMMY_PARAMS = parseBoolEnvVar("LBANN_ONNX_ADD_DUMMY_PARAMS", False)
MB_PLACEHOLDER = "MB"

class TestLbann2Onnx(unittest.TestCase):

    def _test(self, model, inputShapes, testedOutputs=[]):
        modelName = re.compile("^.*?/?([^/.]+).prototext$").search(model).group(1)
        o, miniBatchSize = lbann_onnx.l2o.parseLbannModelPB(model, inputShapes)

        if ADD_DUMMY_PARAMS:
            dummyInits = []
            for i in o.graph.input:
                if i.name in list(map(lambda x: "{}_0".format(x), inputShapes.keys())) or \
                   i.name in list(map(lambda x: x.name, o.graph.initializer)):# TODO: remove "_0"
                    continue

                shape = lbann_onnx.util.getDimFromValueInfo(i)
                dummyInits.append(numpy_helper.from_array(np.zeros(shape, dtype=lbann_onnx.ELEM_TYPE_NP),
                                                          name=i.name))

            g = onnx.helper.make_graph(o.graph.node,
                                       o.graph.name,
                                       o.graph.input,
                                       o.graph.output,
                                       list(o.graph.initializer) + dummyInits,
                                       value_info=o.graph.value_info)
            o = onnx.helper.make_model(g)

        if SAVE_ONNX:
            onnx.save(o, os.path.join(DUMP_DIR, "{}.onnx".format(modelName)))

        for nodeName, outputShape in testedOutputs:
            node = list(filter(lambda x: x.name == nodeName, o.graph.node))
            assert len(node) == 1, node
            outputVI = list(filter(lambda x: x.name == node[0].output[0], o.graph.value_info))
            assert len(outputVI) == 1, outputVI
            outputShapeActual = lbann_onnx.util.getDimFromValueInfo(outputVI[0])
            outputShapeWithMB = list(map(lambda x: miniBatchSize if x == MB_PLACEHOLDER else x, outputShape))
            assert outputShapeWithMB == outputShapeActual, (outputShapeWithMB, outputShapeActual)

    def test_l2o_mnist(self):
        width = 28
        classes = 10
        self._test("{}/simple_mnist/model_mnist_simple_1.prototext".format(LBANN_MODEL_ROOT),
                   {"image": [width*width], "label": [classes]},
                   [("relu1", [MB_PLACEHOLDER, 500]),
                    ("prob", [MB_PLACEHOLDER, classes])])

    def test_l2o_autoencoder_mnist(self):
        width = 28
        self._test("{}/autoencoder_mnist/model_autoencoder_mnist.prototext".format(LBANN_MODEL_ROOT),
                   {"image": [width*width]},
                   [("reconstruction", [MB_PLACEHOLDER, width*width])])

    def test_l2o_vae_mnist(self):
        width = 28
        self._test("{}/autoencoder_mnist/vae_mnist.prototext".format(LBANN_MODEL_ROOT),
                   {"image": [width*width]},
                   [("reconstruction", [MB_PLACEHOLDER, width*width])])

    def test_l2o_alexnet(self):
        width = 224
        classes = 1000
        self._test("{}/alexnet/model_alexnet.prototext".format(LBANN_MODEL_ROOT),
                   {"image": [3, width, width], "label": [classes]},
                   [("relu4", [MB_PLACEHOLDER, 384, 12, 12]),
                    ("prob", [MB_PLACEHOLDER, classes])])

    def test_l2o_resnet50(self):
        width = 224
        classes = 1000
        self._test("{}/resnet50/model_resnet50.prototext".format(LBANN_MODEL_ROOT),
                   {"images": [3, width, width], "labels": [classes]},
                   [("res4a", [MB_PLACEHOLDER, 1024, 14, 14]),
                    ("prob", [MB_PLACEHOLDER, classes])])

    def test_l2o_cosmoflow(self):
        width = 128
        secrets = 3
        self._test("{}/cosmoflow/model_cosmoflow.prototext".format(LBANN_MODEL_ROOT),
                   {"DARK_MATTER": [1, width, width, width], "SECRETS_OF_THE_UNIVERSE": [secrets]},
                   [("act5", [MB_PLACEHOLDER, 256, 4, 4, 4]),
                    ("drop3", [MB_PLACEHOLDER, secrets])])

    def test_l2o_gan_mnist_adversarial(self):
        width = 28
        classes = 2
        self._test("{}/gan/mnist/adversarial_model.prototext".format(LBANN_MODEL_ROOT),
                   {"data": [width, width], "label": [classes]},
                   [("fc4_tanh", [1, width*width]),
                    ("prob", [MB_PLACEHOLDER, 2])])

    def test_l2o_gan_mnist_discriminator(self):
        width = 28
        classes = 2
        self._test("{}/gan/mnist/discriminator_model.prototext".format(LBANN_MODEL_ROOT),
                   {"data": [width, width], "label": [classes]},
                   [("fc4_tanh", [1, width*width]),
                    ("prob", [MB_PLACEHOLDER, 2])])

if __name__ == "__main__":
    unittest.main()
