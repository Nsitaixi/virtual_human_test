#这个文件的作用是：把PyTorch训练好的模型（AudioToBlendshape）转换成ONNX格式。ONNX（Open Neural Network Exchange）是一种通用的深度学习模型格式，可以在不同框架（如 PyTorch、TensorFlow）和推理引擎（如 TensorRT、ONNX Runtime）之间交换模型

from pathlib import Path  #从Python标准库pathlib中导入Path类。Path用于处理文件路径，比直接用字符串拼接更安全、跨平台（Windows/Linux/Mac自动处理斜杠差异）。例如：Path(__file__).resolve().parent可以得到当前文件所在的目录
import torch  #导入PyTorch主库。torch提供了张量操作、神经网络模块、以及模型导出功能torch.onnx.export
from torch_model import AudioToBlendshape  #从同目录下的torch_model.py文件中，导入我们之前定义的AudioToBlendshape这个神经网络类

def main():  #在Python中，通常把主要逻辑放在一个函数里，然后在脚本末尾调用它。这样有利于代码组织和复用。
    out_dir = Path(__file__).resolve().parent  #__file__：一个Python内置变量，表示当前文件的完整路径（例如/home/nsitaixi/workspace/virtual_human_test/models/export_onnx.py）
                                               #Path(__file__)：用Path把字符串路径包装成路径对象
                                               #.resolve()：解析可能存在的符号链接，得到绝对路径（一般会直接调用，更安全）
                                               #.parent：获取父目录。例如/home/.../models/export_onnx.py的父目录是/home/.../models
                                               #out_dir：当前脚本所在的文件夹
    onnx_path = out_dir / "model.onnx"  #out_dir / "model.onnx"：使用/运算符拼接路径，等价于os.path.join(out_dir,"model.onnx")。得到ONNX文件的完整保存路径，例如 /home/.../models/model.onnx

    model = AudioToBlendshape(in_dim=128,out_dim=52).eval()  #创建一个模型对象，输入维度128，输出维度52，并将模型切换到评估模式
                                                             #train()：训练模式，某些层（如Dropout、BatchNorm）的行为不同
                                                             #eval()：评估/推理模式，关掉Dropout、固定BatchNorm的均值和方差。导出ONNX时必须用eval()，保证推理结果确定
    dummy = torch.randn(1,160,128)  #生成一个随机张量。这个张量叫做虚拟输入（dummy input），它并不包含真实数据，只是用来告诉ONNX导出器：“我的模型期望输入的形状大概是这样，但时间维度T可以变化”

    torch.onnx.export(  #调用torch.onnx.export导出模型
        model,  #要导出的PyTorch模型实例（已经处于 eval() 模式）
        dummy,  #示例输入，ONNX导出器会用它运行一次模型，追踪计算图
        str(onnx_path),  #输出ONNX文件的路径（需要字符串形式）。onnx_path是Path对象，用str()转换
        input_names=["audio_feat"],  #为输入节点起一个名字，这里叫"audio_feat"。这个名字会在生成的ONNX文件中作为输入张量的标识。后面在TensorRT或ONNX Runtime中，我们可以通过这个名字来传入数据
        output_names=["blendshape"],  #为输出节点起名字，这里叫"blendshape"。同样用于后续推理时获取结果
        opset_version=18,  #ONNX操作集版本。不同版本支持的算子不同。18是比较新的稳定版本（PyTorch 2.x 支持）。如果不指定，PyTorch会使用默认版本（可能较低）。选择18可以确保较好的兼容性
        do_constant_folding=True,  #是否进行常量折叠优化。如果为True，导出器会预先计算那些不依赖输入数据的常量表达式（比如模型中的固定权重运算），把结果直接写入模型，减少推理时的计算量。一般建议开启
        dynamic_axes={"audio_feat": {1: "T"}}  #定义动态轴。默认情况下，ONNX要求输入形状完全固定。但我们希望模型能够处理不同长度的音频（T可变）。即导出的ONNX模型在推理时，可以接受 (1,T,128)的任何T值（只要T不超过某个上限）
                                               #"audio_feat"：表示对输入节点audio_feat设置动态轴
                                               #{1: "T"}：表示这个张量的第1维（索引从0开始）是动态的，给它起个名字叫"T"
                                               #输出形状如果也依赖于T，则也需要对输出节点blendshape设置动态轴，但这里只指定输入动态即可。（本模型输出与T无关，因为做了mean）
    )

    print(f"Model exported to:{onnx_path}")  #使用f-string格式化输出，打印ONNX文件的保存路径，告诉用户导出成功

if __name__ == "__main__":
    main()