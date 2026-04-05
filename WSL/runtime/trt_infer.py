#这个文件实现了一个TensorRT推理类，负责加载TensorRT引擎文件（.engine），并用它对输入数据进行高速推理，输出面部表情参数（blendshape权重）。TensorRT是NVIDIA推出的高性能深度学习推理优化器，能将模型转换成GPU上极致优化的代码，大幅降低延迟

from __future__ import annotations  #这是Python的一个“未来特性”导入，主要作用是允许在类型注解中使用尚未定义的类

import numpy as np
import tensorrt as trt  #导入TensorRT库，简写为trt。提供加载引擎、创建执行上下文、推理等核心功能
import pycuda.driver as cuda  #导入PyCUDA的驱动模块，简写为cuda。PyCUDA是Python中操作CUDA（NVIDIA的GPU编程框架）的库。我们用它来分配GPU显存、在CPU和GPU之间拷贝数据、同步流等
import pycuda.autoinit #noqa:F401  #自动初始化CUDA上下文。这一行会调用pycuda.driver的初始化代码，设置设备、创建上下文等。没有这一行，后续的 CUDA 操作会失败。# noqa: F401 是告诉代码检查工具忽略“导入但未使用”的警告，因为它是隐式初始化。

TRT_LOGGER = trt.Logger(trt.Logger.WARNING)  #trt.Logger：TensorRT 的日志器，用于输出错误、警告、信息等。
                                             #trt.Logger.WARNING：设置日志级别为 WARNING，只显示警告和错误信息，忽略更详细的信息（INFO、VERBOSE）。这样可以减少控制台输出。
                                             #TRT_LOGGER：将这个日志器实例保存到全局变量 TRT_LOGGER 中，后续加载引擎时需要用到

class TRTInfer:  #定义一个名为 TRTInfer 的类。这个类封装了所有 TensorRT 推理相关的操作：加载引擎、分配显存、执行推理、获取结果。
    def __init__(self,  #def __init__(self, ...)：构造函数，在创建 TRTInfer 对象时自动调用。
            engine_path: str,  #TensorRT 引擎文件的路径（字符串类型）。
            input_names: str = "audio_feat",  #模型中输入张量的名称，默认 "audio_feat"，必须与导出 ONNX 时设置的输入名称一致。
            in_dim: int = 128,  #输入特征维度（每个时间步的特征数），默认 128。
            output_names: str = "blendshape",  #输出张量的名称，默认 "blendshape"，与 ONNX 导出时的输出名称一致。
            out_dim: int = 52,  #输出特征维度（blendshape 数量），默认 52。
            max_T: int = 1600  #最大时间步数（即输入序列的最大长度）。由于我们分配 GPU 显存时需要知道最大可能的大小，这里预设为 1600（约 10 秒音频，以 16kHz、hop_length=160 计算）。
    ):
        self.input_names = input_names  #将参数保存为实例变量（self.xxx），这样类的其他方法（如 infer）也可以访问这些值。
        self.output_names = output_names
        self.in_dim = in_dim
        self.out_dim = out_dim
        self.max_T = max_T

        with open(engine_path, "rb") as f:  #以二进制读模式打开引擎文件。with 语句确保文件用完后自动关闭。
            runtime = trt.Runtime(TRT_LOGGER)  #创建一个 TensorRT 运行时（Runtime）对象，需要传入日志器。Runtime 负责反序列化引擎文件。
            self.engine = runtime.deserialize_cuda_engine(f.read())  #f.read()：读取整个引擎文件的二进制内容。
                                                                     #runtime.deserialize_cuda_engine(...)：将二进制数据反序列化为一个 CUDA 引擎（ICudaEngine）对象，并赋值给 self.engine。这个引擎包含了模型的计算图和优化后的 kernel。

        if self.engine is None:  #检查引擎是否加载成功，如果为 None 则抛出异常。
            raise RuntimeError("Failed to load TensorRT engine:{engine_path}")

        self.context = self.engine.create_execution_context()  #从引擎创建一个 执行上下文（IExecutionContext）。上下文负责管理推理时的资源（如中间缓冲区、绑定输入输出）。一个引擎可以创建多个上下文，用于并行推理。

        if self.context is None:
            raise RuntimeError("Failed to create execution context")

        self.stream = cuda.Stream()  #创建一个 CUDA 流（Stream）。CUDA 操作（内存拷贝、核函数执行）可以放入不同的流中，以实现异步执行和并发。我们用一个流来管理这个推理任务的所有 GPU 操作。

        self.max_input_shape = (1, self.max_T, self.in_dim)  #定义最大输入形状，即 (batch=1, max_T, in_dim)。我们只支持 batch=1。
        self.input_bytes = int(np.prod(self.max_input_shape) * np.dtype(np.float32).itemsize)  #np.prod(self.max_input_shape)：计算形状中各维度的乘积，得到最大元素个数。例如 (1, 1600, 128) 的乘积是 1*1600*128 = 204800。
                                                                                               #np.dtype(np.float32).itemsize：float32 类型每个元素占 4 个字节（32位 = 4字节）。
                                                                                               #最终self.input_bytes 是输入缓冲区需要的最大字节数。int(...)：将结果转为整数。
        self.outshape = (1, self.out_dim)  #输出形状固定为 (1, out_dim)
        self.output_bytes = int(np.prod(self.output_shape) * np.dtype(np.float32).itemsize)  #输出字节数固定为 1 * 52 * 4 = 208 字节。

        self.d_input = cuda.mem_alloc(self.input_bytes)  #cuda.mem_alloc(size)：在 GPU 上分配指定字节数的显存，返回一个指向该显存的“指针”对象（DeviceAllocation）。这里 self.d_input 存放输入数据，self.d_output 存放输出数据。
        self.d_output = cuda.mem_alloc(self.output_bytes)  #注意：这些显存是一次性按最大需求分配的，后续推理时不再重复分配，提高效率。

    def infer(self, feat_np: np.ndarray) -> np.ndarray:  #定义 infer 方法，接收一个 NumPy 数组 feat_np（音频特征），返回 NumPy 数组（blendshape 权重）。
        if feat_np.ndim != 3:  #检查输入必须是 3 维数组
            raise ValueError(f"Expected 3D input, got {feat_np.ndim}")
        if feat_np.shape[0] != 1 or feat_np.shape[2] != self.in_dim:  #检查第一维（batch）必须为 1，第三维必须等于 self.in_dim（128），第二维在后面检查
            raise ValueError(f"Expected shape (1, T, {self.in_dim}), got {feat_np.shape}")

        feat_np = np.ascontiguousarray(feat_np.astype(np.float32))  #feat_np.astype(np.float32)：确保数据类型为 32 位浮点数。
                                                                    #np.ascontiguousarray(...)：保证数组在内存中是连续存储的（C 风格）。CUDA 拷贝需要连续内存，否则可能出错。
        T = int(feat_np.shape[1])  #获取时间步长度 T
        if T > self.max_T:  #检查是否超过预设的最大值 max_T
            raise ValueError(f"T={T} exceeds max_T={self.max_T}")

        self.context.set_input_shape(self.input_name, (1, T, self.in_dim))  #self.context.set_input_shape(input_name, shape)：告诉 TensorRT 上下文，实际输入的形状是多少（动态形状）。因为我们在导出 ONNX 时设置了动态轴（dynamic_axes），这里必须设置实际形状。
        self.context.set_tensor_address(self.input_name, int(self.d_input))  #set_tensor_address(name, address)：将输入/输出张量绑定到之前分配的 GPU 显存地址。
        self.context.set_tensor_address(self.output_name, int(self.d_output))  #int(self.d_input) 获取 GPU 指针的整数值。

        cuda.memcpy_htod_async(self.d_input, feat_np, self.stream)  #cuda.memcpy_htod_async(dst, src, stream)：将 CPU 上的数据 feat_np 异步拷贝到 GPU 显存 self.d_input 中，使用指定的流 self.stream。
                                                                    #htod 表示 Host（CPU）To Device（GPU）。
                                                                    #异步意味着拷贝操作不会阻塞 CPU，可以继续执行后续操作（但这里我们紧接着就启动推理，实际上推理会等待拷贝完成，因为它们在同一个流中）。

        ok = self.context.execute_async_v3(stream_handle=self.stream.handle)  #self.context.execute_async_v3(...)：在 GPU 上异步执行推理。 _v3 是较新的 API 版本，与之前版本的 execute_async 略有不同，但功能相同。
                                                                              #stream_handle：指定使用哪个流。TensorRT 会从绑定的输入地址读取数据，计算后写入输出地址。
                                                                              # ok 是布尔值，表示执行是否成功。
        if not ok:
            raise RuntimeError("TensorRT execute_async_v3 failed")

        out = np.empty(self.output_shape, dtype=np.float32)  #在 CPU 上分配一个空的 NumPy 数组，形状为输出形状，用于存放结果。
        cuda.memcpy_dtoh_async(out, self.d_output, self.stream)  #将 GPU 显存 self.d_output 中的数据异步拷贝到 CPU 数组 out 中，使用同一个流。
        self.stream.synchronize()  #阻塞 CPU，直到该流中的所有操作（拷贝输入、推理、拷贝输出）全部完成。这是必需的，否则 out 可能还未拷贝完成就被返回，导致数据不完整
        return out  #返回包含 blendshape 权重的 NumPy 数组，形状 (1, 52)