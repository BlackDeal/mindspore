# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from mindspore import Tensor
import mindspore as ms
import numpy as np
from mindspore.ops import operations as P
import mindspore.nn as nn
from mindspore.common.parameter import Parameter
from tests.dataset_mock import MindData
from mindspore import context
from mindspore.train import Model, ParallelMode
from mindspore.nn.optim import Momentum


context.set_context(mode=context.GRAPH_MODE)


class Dataset(MindData):
    def __init__(self, predict, label, length=3):
        super(Dataset, self).__init__(size=length)
        self.predict = predict
        self.label = label
        self.index = 0
        self.length = length

    def __iter__(self):
        return self

    def __next__(self):
        if self.index >= self.length:
            raise StopIteration
        self.index += 1
        return self.predict, self.label

    def reset(self):
        self.index = 0


class CommonNet(nn.Cell):
    def __init__(self):
        super(CommonNet, self).__init__()
        self.weight = Parameter(Tensor(np.ones([256, 64]), dtype=ms.float32), name="mul_weight")
        self.logicalnot = P.LogicalNot().set_strategy(((4,1),))
        self.equal = P.Equal().set_strategy(((4,2),(4,2)))

    def construct(self, x, label):
        x = self.equal(x, self.weight)
        x = self.logicalnot(x)
        return x


def common_net():
    epoch_size = 1

    context.reset_auto_parallel_context()
    
    context.set_auto_parallel_context(parallel_mode="semi_auto_parallel", device_num=8)
    predict = Tensor(np.ones([32, 64]), dtype=ms.float32)
    label = Tensor(np.ones([32]), dtype=ms.int32)
    dataset = Dataset(predict, label, 2)
    net = CommonNet()

    optimizer = Momentum(net.trainable_params(), learning_rate=0.1, momentum=0.9)
    model = Model(net, optimizer=optimizer)
    model.train(epoch_size, dataset, dataset_sink_mode=False)


def test_bool_grad():
    common_net()