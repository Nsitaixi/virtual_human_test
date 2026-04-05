#这个文件定义了一个神经网络模型，它的作用是把音频特征转换成面部表情参数（blendshape 权重）。它使用PyTorch框架实现，是一个简单的全连接网络（也叫多层感知机，MLP）

import  torch  #导入PyTorch主库。PyTorch是一个深度学习框架，提供张量计算、自动求导、神经网络层等功能
import torch.nn as nn  #导入PyTorch的神经网络子模块，并给它起一个简短的别名nn。nn包含了构建神经网络所需的所有“积木块”，比如线性层（nn.Linear）、激活函数（nn.ReLU）等

class AudioToBlendshape(nn.Module):  #声明一个名为AudioToBlendshape的类。括号表示继承自nn.Module。在PyTorch中，所有神经网络模型都必须继承nn.Module，这样PyTorch才知道这是一个可训练的模型，并自动管理参数、前向传播等
    def __init__(self,in_dim=128,out_dim=52):  #def __init__(self, ...)：这是Python类的构造函数。当创建一个AudioToBlendshape对象时，会自动调用这个方法，用来初始化模型的内部结构
                                               #in_dim=128：输入维度，默认值为 128。代表输入特征向量的长度。这里的128是音频mel特征（梅尔频谱）的维度
                                               #out_dim=52：输出维度，默认值为 52。代表输出的blendshape权重的数量。52是标准ARKit面部表情参数的数量
        super().__init__()  #super()：调用父类（nn.Module）的方法
                            #.__init__()：执行父类的构造函数。这一步是必须的，因为nn.Module内部要做很多初始化工作（比如注册参数、准备计算图等）。不写这一行，模型可能无法正常工作
        self.net = nn.Sequential(  #self.net：我们给这个模型内部的一个组件起名为net。self表示这个变量属于整个对象，可以在类的其他方法（比如下面的forward）中访问
                                   #nn.Sequential：这是一个容器，它按顺序包装多个网络层。输入数据会依次经过容器里的每一层，前一层的输出自动成为下一层的输入。非常适合搭建线性堆叠的网络
            nn.Linear(in_dim,256),  #创建一个全连接层（也叫线性层、稠密层）。这里将输入的128维特征映射到更高维的256维空间，提取更丰富的特征
            nn.ReLU(),  #ReLU激活函数，全称修正线性单元。如果输入是正数就会通过，是负数则被清零。这样做可以让网络学会只关注有用的信号，忽略负面的干扰，同时引入非线性。如果没有激活函数，多层线性层最终等价于一个线性层，表达能力很弱，ReLU能帮助网络学习复杂的模式。且ReLU的计算简单，能缓解梯度消失，且能让部分神经元“死亡”（输出 0），产生稀疏性
            nn.Linear(256,128),  #第二个全连接层，将特征从高维空间再压缩回128维，提取更紧凑的表示
            nn.ReLU(),
            nn.Linear(128,out_dim),  #第三个全连接层，将特征映射到目标维度——即52个blendshape权重
            nn.Sigmoid(),  #Sigmoid激活函数，将输出值压缩到0和1之间。因为blendshape权重通常表示肌肉运动的强度，范围一般是0（无运动）到1（最大运动）
        )

    def forward(self,x):  #def forward(self, ...)：定义前向传播函数。这是nn.Module要求我们实现的方法。当把输入数据x传给模型对象时（例如 m(x)），PyTorch会自动调用这个forward方法
                          #x：输入数据，通常是一个张量（Tensor），形状为 (batch_size, sequence_length, in_dim)，即（批次大小，时间步数，特征维度）。在本例中，期望形状是 (1,T,128)，其中T是音频帧数
        x = x.mean(dim=1)  #对输入张量x在维度1上求平均值（即对时间维度做平均）。原始形状(1,T,128)操作后形状变为(1,128)。去掉了时间维度，把整段音频的所有帧特征压缩成一个“平均特征向量”。因为输入音频长度T是可变的（不同句子长度不同），而模型要求输入维度固定。通过对时间取平均，无论T是多少，都得到128维的固定向量。这是一种简单的池化操作
        return self.net(x)  #self.net(x)将平均后的特征x（形状(1,128)）输入到之前定义好的Sequential容器（self.net(x)）中，依次经过三个全连接层和激活函数，得到最终输出。输出形状为(1,out_dim)，即(1,52)，每个值是0~1之间的blendshape权重

if __name__ == "__main__":  #如果这个脚本是直接运行的（比如在终端输入python torch_model.py），那么执行下面的代码块；如果这个文件被其他文件导入（import torch_model），则不执行。这样可以方便地写一些测试代码，同时不影响作为模块导入时的行为
    m = AudioToBlendshape()  #创建一个AudioToBlendshape类的实例，赋值给变量m。这里使用了默认的in_dim=128和out_dim=52
    x = torch.randn(1,160,128)  #torch.randn(1,160,128)：生成一个形状为 (1,160,128) 的随机张量，元素服从标准正态分布（均值为0，方差为1）。这个随机张量用来模拟真实的音频特征输入，方便测试模型是否能正常运行
                                #1：批次大小（batch size），表示一次处理1个样本
                                #160：时间步数（T），模拟一段音频有160帧。在实际中，160帧大约对应1秒（如果hop_length=160，采样率16kHz）
                                #128：每个时间步的特征维度
    print(m(x).shape)  #m(x)：调用模型的前向传播，传入随机输入x。因为m是nn.Module的子类，所以m(x)会自动调用m.forward(x)
                       #.shape：打印输出张量的形状。预期输出是torch.Size([1, 52])，表示1个样本，52个blendshape权重