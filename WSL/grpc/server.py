#这份文件实现了一个 gRPC 服务端，用于接收客户端的音频或文本请求，通过多种后端（TTS、面部表情模型、身体动作模型、情绪识别模型）生成虚拟人的表情、动作、情绪和合成语音，并将结果返回给客户端。

from __future__ import annotations  #这是一个 Python 的“未来特性”导入，允许在类型注解中使用尚未定义的类

import io  #导入 Python 内置的 io 模块，用于处理字节流、内存中的文件等。后面会用 io.BytesIO 来模拟文件对象，处理 WAV 数据。
import os  #导入操作系统接口模块，虽然在这个片段里没有直接使用，但可能在后续代码中用到。
import sys  #导入系统相关模块，用于修改 Python 路径（sys.path.insert），以便导入其他目录的模块。
import json  #用于解析和生成 JSON 数据，与 LLM API 通信时需要
import threading  #提供线程锁（threading.Lock），用于保护历史对话记录的并发访问（多线程环境下安全）
from collections import deque  #导入双端队列（deque），用于存储对话历史，可以限制最大长度（自动丢弃旧消息）
from dataclasses import dataclass  #从 dataclasses 模块导入 dataclass 装饰器。它用于创建数据类，可以自动生成 __init__、__repr__ 等方法，简化代码。
from pathlib import Path  #导入 Path 类，用于处理文件路径，比字符串拼接更安全、跨平台。
from concurrent import futures  #导入 futures 模块，用于创建线程池，gRPC 服务器需要它来并发处理请求。
from typing import Optional, Tuple  #导入类型提示中的 Optional（表示可能为 None）和 Tuple（元组），用于给变量和函数添加类型注解，提高代码可读性。
from urllib import error as urlerror  #导入 urllib 的错误模块，用于捕获网络请求异常（虽然代码中没有显式使用，但 urlopen 可能抛出异常，被外层 try...except 捕获）
from urllib import request as urlrequest  #导入 urllib 的请求模块，用于发送 HTTP 请求到 LLM API。

import grpc  #导入 gRPC 库，用于创建 gRPC 服务器和客户端。
import librosa  #导入 librosa，一个音频处理库，用于重采样、提取梅尔频谱等
import numpy as np
import soundfile as sf  #导入 soundfile，简写为 sf，用于读写 WAV 文件（包括从字节流中读取）。

PROJECT_ROOT = Path(__file__).resolve().parents[1]  #__file__ 是当前文件（server.py）的路径。
                                                    #Path(__file__) 将其转为 Path 对象
                                                    #.resolve() 解析为绝对路径
                                                    #.parents[1] 获取父目录的父目录，即项目的根目录（因为 server.py 通常放在 grpc 文件夹下，而项目根目录是上一级的上级）。例如，如果 server.py 在 /home/user/project/grpc/server.py，那么 PROJECT_ROOT 就是 /home/user/project。
sys.path.insert(0, str(PROJECT_ROOT))  #将项目根目录添加到 Python 的模块搜索路径的最前面，这样后面就可以直接 import 根目录下的其他模块（如 avatar_stream_pb2、runtime.trt_infer）。

import avatar_stream_pb2 as pb  #导入由 protobuf 编译生成的 avatar_stream_pb2 模块，里面包含了 StreamRequest、StreamResponse 等消息类，并给它起别名 pb
import avatar_stream_pb2_grpc as pb_grpc  #导入由 protobuf 编译生成的 gRPC 服务模块，里面包含 AvatarServiceServicer 基类等，并起别名 pb_grpc
from runtime.trt_infer import TRTInfer  #从项目根目录下的 runtime/trt_infer.py 文件中导入 TRTInfer 类，用于加载 TensorRT 引擎并进行面部表情推理。

HOME = Path.home()  #获取当前用户的家目录（例如 /home/username），用于定位用户目录下的资源。
DEFAULT_FACE_ENGINE = PROJECT_ROOT / "models" / "model.engine"  #定义一个默认的 TensorRT 引擎文件路径，位于项目根目录下的 models 文件夹，文件名 model.engine。注意 Path 对象可以用 / 运算符拼接路径
DEFAULT_FACE_ONNX = HOME / "Audio2Face-3D-v3.0" / "network.onnx"  #定义默认的 ONNX 模型文件路径，位于用户家目录下的 Audio2Face-3D-v3.0 文件夹中。
DEFAULT_EMOTION_ONNX = HOME / "Audio2Emotion-v3.0" / "network.onnx"  #类似，情绪识别的 ONNX 模型路径
DEFAULT_PANTO_REPO = HOME / "PantoMatrix"  #定义 PantoMatrix（身体动作模型）的仓库目录

ARKit_52_NAMES = [  #这是一个 Python 列表，包含了 ARKit 定义的 52 个面部 blendshape 的标准名称。当模型输出 52 个权重时，会按照这个顺序将权重与名称对应起来，返回给客户端。这样客户端就知道每个权重控制的是哪个面部区域
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
    "cheekPuff", "cheekSquintLeft", "cheekSquintRight", "eyeBlinkLeft", "eyeBlinkRight",
    "eyeLookDownLeft", "eyeLookDownRight", "eyeLookInLeft", "eyeLookInRight", "eyeLookOutLeft",
    "eyeLookOutRight", "eyeLookUpLeft", "eyeLookUpRight", "eyeSquintLeft", "eyeSquintRight",
    "eyeWideLeft", "eyeWideRight", "jawForward", "jawLeft", "jawOpen", "jawRight",
    "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight",
    "mouthFunnel", "mouthLeft", "mouthLowerDownLeft", "mouthLowerDownRight", "mouthPressLeft",
    "mouthPressRight", "mouthPucker", "mouthRight", "mouthRollLower", "mouthRollUpper",
    "mouthShrugLower", "mouthShrugUpper", "mouthSmileLeft", "mouthSmileRight", "mouthStretchLeft",
    "mouthStretchRight", "mouthUpperUpLeft", "mouthUpperUpRight", "noseSneerLeft", "noseSneerRight",
    "tongueOut",
]


def decode_wav_bytes(wav_bytes: bytes, target_sr: int = 16000) -> np.ndarray:  #将 WAV 格式的字节数据解码成指定采样率的单声道音频数组。
                                                                               #wav_bytes: bytes：WAV 文件的原始字节
                                                                               #arget_sr: int = 16000：目标采样率（Hz），默认为 16000
                                                                               #返回：np.ndarray，一维浮点数组，表示音频波形
    with io.BytesIO(wav_bytes) as bio:  #用字节流创建一个内存中的文件对象，供 sf.read 读取。
        audio, sr = sf.read(bio, dtype="float32", always_2d=False)  #使用 soundfile 读取 WAV 数据。dtype="float32" 将样本转为浮点数（范围 -1 到 1）；always_2d=False 表示单声道时返回一维数组。

    if audio is None or len(audio) == 0:  #如果读取失败或音频为空，返回一个空数组。
        return np.zeros((0,), dtype=np.float32)

    if audio.ndim == 2:  #如果音频是多声道（二维），则取所有声道的平均值转为单声道。
        audio = audio.mean(axis=1)

    if sr != target_sr:  #如果原始采样率不是目标采样率，用 librosa.resample 进行重采样。
        audio = librosa.resample(audio, orig_sr=sr, target_sr=target_sr)

    return np.ascontiguousarray(audio.astype(np.float32))  #确保数组是连续内存布局，并转为 float32 类型后返回。


def pcm_to_wav_bytes(audio: np.ndarray, sr: int) -> bytes:  #将 PCM 音频数据（浮点数组）包装成 WAV 格式的字节。
                                                            #audio: np.ndarray：音频数据
                                                            #sr: int：采样率
                                                            #返回：bytes，WAV 文件的字节
    audio = np.asarray(audio, dtype=np.float32)  #确保是 float32 数组
    if audio.ndim != 1:  #如果多声道，展平为一维
        audio = audio.reshape(-1)

    buf = io.BytesIO()  #创建一个内存中的字节缓冲区
    sf.write(buf, audio, sr, format="WAV", subtype="PCM_16")  #用 soundfile 将音频写入缓冲区，保存为 16 位 PCM 的 WAV 格式。
    return buf.getvalue()  #获取缓冲区的字节数据并返回。


def resample_audio(audio: np.ndarray, src_sr: int, dst_sr: int) -> np.ndarray:  #重采样音频数组到目标采样率。
    if src_sr == dst_sr:  #如果源和目标采样率相同，直接转为 float32 返回；否则调用 librosa.resample 进行重采样。
        return np.asarray(audio, dtype=np.float32)
    return librosa.resample(np.asarray(audio, dtype=np.float32), orig_sr=src_sr, target_sr=dst_sr)


def make_mel_features(audio_16k: np.ndarray) -> np.ndarray:  #从 16kHz 的音频中提取梅尔频谱特征，并整理成模型输入所需的形状。
    feat = librosa.feature.melspectrogram(  #调用 librosa.feature.melspectrogram
        y=audio_16k,  #音频信号。
        sr=16000,  #采样率
        n_mels=128,  #梅尔频带数量，即特征维度
        n_fft=400,  #FFT 窗口大小
        hop_length=160,  #帧移（10ms，因为 160/16000 = 0.01s）。
        win_length=400,  #窗口长度，通常等于 n_fft
        power=2.0,  #对幅度平方，得到能量谱
        center=False,  #不进行中心填充，使得输出长度与输入帧数匹配
    )
    return feat.T[np.newaxis, :, :].astype(np.float32)  #feat 的形状是 (n_mels, T)，其中 T 是时间帧数。
                                                        #feat.T 转置为 (T, n_mels)
                                                        #[np.newaxis, :, :] 增加一个 batch 维度，变为 (1, T, n_mels)
                                                        #.astype(np.float32) 转为 float32 类型


def softmax(x: np.ndarray) -> np.ndarray:  #计算 softmax 函数，将输入向量转为概率分布。这里用于情绪分类的 logits 转换为概率
    x = np.asarray(x, dtype=np.float32)  #确保为 float32 数组。
    x = x - np.max(x)  #减去最大值，防止指数溢出（数值稳定）。
    e = np.exp(x)  #计算指数
    denom = np.sum(e)  #所有指数的和
    if denom <= 0:  #如果和为 0（异常情况），返回全零数组
        return np.zeros_like(x)
    return e / denom  #返回概率分布


@dataclass  #这是 Python 3.7+ 引入的装饰器，自动为类添加 __init__、__repr__ 等方法，简化代码。
class InferenceOutputs:  #定义一个用于封装推理结果的数据类
    wav: bytes = b""  #如果请求需要音频输出（例如文本转语音的结果），这个字段存放合成的 WAV 文件字节
    face_names: Tuple[str, ...] = ()  #面部 blendshape 的名称列表，通常为 ARKit 的 52 个名称
    face_weights: Tuple[float, ...] = ()  #与 face_names 一一对应的权重值，范围 0~1
    body_channel_names: Tuple[str, ...] = ()  #身体动作通道的名称（如骨骼名称）
    body_values: Tuple[float, ...] = ()  #与 body_channel_names 对应的数值（如轴角旋转值）
    emotion: str = "neutral"  #识别出的情绪标签
    status: str = "ok"  #处理状态，成功时为 "ok"，失败时包含错误信息
    reply_text: str = ""  #在文本模式下，记录 LLM 生成的回复文本（可能是经过智能改写或补全的句子），方便调试或返回给客户端


class ChatTTSBackend:  #这个类封装了 ChatTTS 文本转语音引擎，用于将文本合成为音频波形
    def __init__(self) -> None:
        self._chat = None  #存储 ChatTTS 的实例对象，初始为 None
        self.enabled = False  #标记该后端是否成功加载，初始为 False
        try:  #尝试导入并初始化 ChatTTS
            import ChatTTS  # type: ignore  #导入 ChatTTS 库（需要提前安装）。# type: ignore 用于忽略类型检查警告。
            import torch  # type: ignore  #导入 PyTorch，因为 ChatTTS 依赖它。

            self._torch = torch  #将 torch 保存为实例变量，可能在后续推理中使用（虽然此处未使用，但保留）。
            self._chat = ChatTTS.Chat()  #创建 ChatTTS 的聊天对象
            if hasattr(self._chat, "load_models"):  #检查对象是否有 load_models 方法（某些版本使用此方法加载模型）。
                self._chat.load_models()  #加载预训练模型
            elif hasattr(self._chat, "load"):  #否则检查是否有 load 方法。先尝试 load(compile=False) 调用（避免编译），如果因参数不匹配抛出 TypeError，则回退到无参数 load()
                try:
                    self._chat.load(compile=False)
                except TypeError:
                    self._chat.load()
            self.enabled = True  #若加载成功，将 enabled 设为 True
            print("[Server] ChatTTS loaded")  #打印成功信息
        except Exception as exc:  #捕获任何异常（如库未安装、模型下载失败等）
            print(f"[Server] ChatTTS disabled: {exc}")  #打印禁用信息，方便调试

    def synthesize(self, text: str) -> Tuple[np.ndarray, int]:  #text: str 要合成的文本。返回音频数组（float32）和采样率（24000）
        if not self.enabled or self._chat is None:  #检查后端是否可用，若不可用则抛出异常
            raise RuntimeError("ChatTTS backend is not available")  #

        wavs = self._chat.infer([text], use_decoder=True)  #调用 ChatTTS 的 infer 方法，传入文本列表（这里只有一个文本），use_decoder=True 表示使用解码器生成语音。返回的是一个列表，每个元素对应一个文本的音频张量（通常是 PyTorch Tensor）。
        wav = wavs[0]  #取出第一个（也是唯一一个）音频张量
        if hasattr(wav, "detach"):  #如果对象有 detach 方法（即它是 PyTorch Tensor），则将其从计算图中分离，并转为 NumPy 数组。
            wav = wav.detach().cpu().numpy()
        elif hasattr(wav, "cpu"):  #否则如果有 cpu 方法（可能是其他框架的张量），也尝试转为 CPU 并转为 NumPy。
            wav = wav.cpu().numpy()  #

        wav = np.asarray(wav, dtype=np.float32)  #确保是 float32 类型的 NumPy 数组
        if wav.ndim > 1:  #如果音频是多通道（例如二维），则展平为一维（单声道）
            wav = wav.reshape(-1)  #

        return wav, 24000  #返回音频数组和固定的采样率 24000 Hz


class LLMBackend:  #
    def __init__(self) -> None:  #
        self.enabled = os.getenv("LLM_ENABLED", "1").strip().lower() not in {"0", "false", "no", "off"}  #从环境变量 LLM_ENABLED 读取是否启用 LLM。默认值为 "1"，如果设置为 "0"、"false"、"no"、"off"（不区分大小写）则禁用。最终 self.enabled 为 True 或 False
        self.provider = os.getenv("LLM_PROVIDER", "openai_compatible").strip().lower()  #从环境变量 LLM_PROVIDER 读取提供者类型，默认 "openai_compatible"（支持 OpenAI API 格式的接口）
        self.api_url = os.getenv("LLM_API_URL", "").strip()  #API 端点地址，例如 https://api.openai.com/v1/chat/completions
        self.api_key = os.getenv("LLM_API_KEY", "").strip()  #API 密钥
        self.model = os.getenv("LLM_MODEL", "deepseek-chat").strip()  #模型名称，默认deepseek-chat
        self.system_prompt = os.getenv(  #系统提示词（system prompt），用于设定 LLM 的角色和行为。默认是友好的虚拟人，回复自然简洁，默认使用中文
            "LLM_SYSTEM_PROMPT",
            "You are a friendly virtual human. Reply naturally, briefly, and in Chinese unless the user writes in another language.",
        ).strip()
        self.temperature = float(os.getenv("LLM_TEMPERATURE", "0.7"))  #控制生成随机性，默认 0.7
        self.max_tokens = int(os.getenv("LLM_MAX_TOKENS", "256"))  #最大生成 token 数，默认 256
        self.timeout = float(os.getenv("LLM_TIMEOUT", "30"))  #HTTP 请求超时秒数，默认 30
        self.history_turns = int(os.getenv("LLM_HISTORY_TURNS", "6"))  #保留的对话轮数（每轮包含用户和助手各一条），默认 6 轮。历史记录用于维持上下文
        self._lock = threading.Lock()  #线程锁，防止多线程同时修改历史记录
        self._history = deque(maxlen=max(0, self.history_turns) * 2)  #双端队列，最大长度为 history_turns * 2（因为一轮有两个消息：user + assistant）。max(0, ...) 确保不会为负数

        if self.enabled:  #根据配置打印日志：如果启用了 LLM 且提供了有效的 API URL，显示正常模式；否则显示 echo 模式（即不调用 LLM，直接返回用户原话）
            if self.provider == "openai_compatible" and self.api_url:
                print(f"[Server] LLM backend enabled: {self.provider} @ {self.api_url}")
            else:
                print(f"[Server] LLM backend enabled in echo mode (provider={self.provider}, api_url={self.api_url or 'unset'})")
        else:
            print("[Server] LLM backend disabled, falling back to raw text")

    @staticmethod  #
    def _sanitize_text(text: str) -> str:  #静态方法，用于清理文本：将连续的空白字符（空格、换行、制表符等）替换为单个空格，然后去掉首尾空格
        text = " ".join((text or "").split())  #如果输入为 None 或空，text or "" 会得到空字符串
        return text.strip()

    def _history_messages(self) -> list[dict[str, str]]:  #构造发送给 LLM 的消息列表：先加入系统提示（如果有），然后加入历史记录
        messages: list[dict[str, str]] = []
        if self.system_prompt:
            messages.append({"role": "system", "content": self.system_prompt})  #返回格式符合 OpenAI Chat Completion API：[{"role": "system", "content": ...}, {"role": "user", ...}, {"role": "assistant", ...}]
        messages.extend(list(self._history))
        return messages

    def _append_turn(self, user_text: str, assistant_text: str) -> None:  #将一轮对话（用户输入和助手回复）加入历史队列
        user_text = self._sanitize_text(user_text)
        assistant_text = self._sanitize_text(assistant_text)
        if not user_text and not assistant_text:  #先清理文本，如果两者都为空则直接返回
            return
        with self._lock:  #使用 with self._lock 确保线程安全。分别添加 user 和 assistant 消息（顺序重要）。由于 deque 限制了最大长度，超出会自动丢弃最旧的消息
            if user_text:
                self._history.append({"role": "user", "content": user_text})
            if assistant_text:
                self._history.append({"role": "assistant", "content": assistant_text})

    def _call_openai_compatible(self, user_text: str) -> str:  #构造 HTTP POST 请求，调用 LLM API，并解析返回的 JSON，提取回复文本
        if not self.api_url:  #如果没有配置 API 地址，直接回显用户输入（相当于 echo 模式）
            return user_text

        payload = {  #构造请求体
            "model": self.model,
            "messages": self._history_messages() + [{"role": "user", "content": user_text}],  #由历史消息 + 当前用户消息组成
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
            "stream": False,  #不使用流式输出
        }

        body = json.dumps(payload).encode("utf-8")  #json.dumps(payload) 转为 JSON 字符串，编码为 UTF-8 字节
        headers = {"Content-Type": "application/json"}  #设置 HTTP 头
        if self.api_key:  #如果提供了 API Key 则添加 Authorization 头
            headers["Authorization"] = f"Bearer {self.api_key}"

        req = urlrequest.Request(self.api_url, data=body, headers=headers, method="POST")  #urlrequest.Request 创建请求对象，指定 POST 方法
        with urlrequest.urlopen(req, timeout=self.timeout) as resp:  #发送请求并获取响应（使用 timeout 参数）
            raw = resp.read().decode("utf-8", errors="replace")

        data = json.loads(raw)  #读取响应字节
        choices = data.get("choices") or []  #解码为字符串
        if choices:  #尝试多种常见的响应格式
            choice0 = choices[0] or {}  #OpenAI 格式：choices[0].message.content
            message = choice0.get("message") or {}
            content = message.get("content")
            if isinstance(content, str) and content.strip():
                return content.strip()

            text = choice0.get("text")  #某些兼容服务可能用 choices[0].text
            if isinstance(text, str) and text.strip():
                return text.strip()

        reply = data.get("reply") or data.get("response") or data.get("output_text")  #或者顶层字段 reply、response、output_text
        if isinstance(reply, str) and reply.strip():
            return reply.strip()

        raise RuntimeError(f"Unexpected LLM response schema: {data!r}")  #如果所有尝试都失败，抛出异常

    def generate(self, user_text: str) -> str:
        user_text = self._sanitize_text(user_text)  #清理输入文本
        if not user_text:  #如果为空则返回空字符串
            return ""

        if not self.enabled:  #如果 LLM 未启用，直接返回用户原文本（echo）
            return user_text

        try:
            if self.provider == "openai_compatible" and self.api_url:  #如果启用且provider匹配且有API URL，则调用API，目前只实现了openai_compatible
                reply = self._call_openai_compatible(user_text)
            else:  #如果调用失败则回显
                reply = user_text
        except Exception as exc:  #若API调用异常，捕获异常并回显
            print(f"[Server] LLM generation failed, fallback to echo: {exc}")
            reply = user_text

        reply = self._sanitize_text(reply)  #再次清理回复文本，如果为空则使用原文本
        if not reply:
            reply = user_text

        self._append_turn(user_text, reply)  #将本轮对话加入历史，并返回回复文本
        return reply


class OnnxAudioBackend:  #这个类封装了 ONNX 模型 的加载和推理，用于面部表情或情绪识别
    def __init__(self, model_path: Path, fallback_name: str) -> None:  #model_path: Path：ONNX 模型文件的路径
                                                                       #fallback_name: str：一个备用的输出名称，如果无法从模型中获取输出名称，则使用这个
        self.model_path = model_path  #保存模型路径
        self.fallback_name = fallback_name  #保存备用输出名称
        self.session = None  #ONNX Runtime 会话，初始 None
        self.input_name = "audio_feat"  #默认输入名称
        self.output_name = fallback_name  #默认输出名称（使用传入的备用名）
        self.enabled = False  #标记是否加载成功

        if not model_path.exists():  #如果模型文件不存在，直接返回（不启用）
            return

        try:  #尝试加载 ONNX 模型
            import onnxruntime as ort  # type: ignore  #导入 ONNX Runtime

            self.session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])  #创建推理会话，指定使用 CPU 执行提供者。str(model_path) 将 Path 转为字符串
            if self.session.get_inputs():  #如果会话有输入节点，则取第一个输入的名称更新 self.input_name
                self.input_name = self.session.get_inputs()[0].name
            if self.session.get_outputs():  #类似，更新 self.output_name
                self.output_name = self.session.get_outputs()[0].name
            self.enabled = True  #标记启用
            print(f"[Server] ONNX backend loaded: {model_path}")  #打印成功信息
        except Exception as exc:  #捕获异常，打印禁用信息
            print(f"[Server] ONNX backend disabled for {model_path}: {exc}")

    def infer(self, audio_16k: np.ndarray) -> np.ndarray:  #参数：audio_16k: np.ndarray，16kHz 采样率的音频波形
                                                           #返回：np.ndarray，模型输出的特征（例如 52 个 blendshape 权重或情绪 logits）
        if not self.enabled or self.session is None:  #检查后端是否可用，不可用则抛异常
            raise RuntimeError(f"ONNX backend unavailable: {self.model_path}")

        feat = make_mel_features(audio_16k)  #调用前面定义的 make_mel_features 函数，从音频提取梅尔频谱特征，形状为 (1, T, 128)。
        outputs = self.session.run([self.output_name], {self.input_name: feat})  #运行 ONNX 推理
                                                                                 #第一个参数是输出名称列表（我们只取一个输出）
                                                                                 #第二个参数是输入字典，键为输入名称，值为输入数据
                                                                                 #返回的 outputs 是一个列表，每个元素对应一个输出张量（NumPy 数组）
        out = np.asarray(outputs[0], dtype=np.float32)  #取出第一个输出，转为 float32 数组
        return out  #返回结果


class PantoMatrixBackend:  #这个类封装了 PantoMatrix 模型（身体动作生成模型），用于根据音频预测身体骨骼旋转值
    def __init__(self) -> None:
        self.enabled = False  #是否启用
        self.model = None  #存储模型实例
        self.cfg = None  #存储模型配置（如音频采样率、帧率等）

        try:  #尝试加载 PantoMatrix 模型
            from models.camn_audio import CamnAudioModel  # type: ignore  #从 models.camn_audio 模块导入模型类（需要提前安装或项目中有该模块）。

            self.model = CamnAudioModel.from_pretrained("H-Liu1997/camn_audio")  #使用 HuggingFace 模型库中的预训练模型（通过名称自动下载）。
            try:  #尝试导入 PyTorch 并设置设备
                import torch  # type: ignore  #导入 PyTorch
                self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")  #根据是否有 GPU 选择设备。
                self.model = self.model.to(self.device)  #将模型移动到指定设备
            except Exception:  #如果导入 torch 或移动设备失败，设置 self.device = None
                self.device = None
            self.model.eval()  #将模型设置为评估模式
            self.cfg = self.model.cfg  #获取模型的配置（包含音频采样率、帧率等参数）
            self.enabled = True  #标记启用
            print("[Server] PantoMatrix loaded")  #打印成功信息
        except Exception as exc:  #捕获异常，打印禁用信息
            print(f"[Server] PantoMatrix disabled: {exc}")

    def infer(self, audio_24k: np.ndarray) -> Tuple[np.ndarray, Tuple[str, ...]]:  #参数：audio_24k: np.ndarray，24kHz 采样率的音频波形
                                                                                   #返回：Tuple[np.ndarray, Tuple[str, ...]]，返回一维扁平化的运动数值数组和对应的通道名称元组。
        if not self.enabled or self.model is None or self.cfg is None:  #检查后端可用性
            raise RuntimeError("PantoMatrix backend is not available")

        import torch  # type: ignore  #导入 PyTorch

        sr = int(getattr(self.cfg, "audio_sr", 24000))  #音频采样率，默认 24000
        pose_fps = int(getattr(self.cfg, "pose_fps", 30))  #姿势帧率，默认 30
        seed_frames = int(getattr(self.cfg, "seed_frames", 1))  #种子帧数，默认 1

        audio = resample_audio(audio_24k, 24000, sr)  #将输入的 24kHz 音频重采样到模型所需的采样率 sr。
        audio_t = torch.from_numpy(audio).float()  #将 NumPy 数组转为 PyTorch 张量，类型为 float32
        if hasattr(self, "device") and self.device is not None:  #如果设备存在，则将音频张量移动到该设备。
            audio_t = audio_t.to(self.device)
        audio_t = audio_t.unsqueeze(0)  #增加 batch 维度，变为 (1, 音频长度)。

        speaker_id = torch.zeros(1, 1).long()  #创建说话人 ID 张量，全 0，形状 (1, 1)，类型为 long
        if hasattr(self, "device") and self.device is not None:  #将 speaker_id 也移动到相应设备
            speaker_id = speaker_id.to(self.device)

        with torch.no_grad():  #禁用梯度计算，节省内存和计算
            motion_pred = self.model(audio_t, speaker_id, seed_frames=seed_frames, seed_motion=None)["motion_axis_angle"]  #调用模型，传入音频、说话人 ID、种子帧数等参数。模型返回一个字典，我们取出键为 "motion_axis_angle" 的值（通常是预测的骨骼轴角数据）。

        motion_np = motion_pred.detach().cpu().numpy()  #将结果从计算图分离、移到 CPU、转为 NumPy 数组。
        flat = motion_np.reshape(-1).astype(np.float32)  #将数组展平为一维，并转为 float32

        channel_names = tuple(f"motion_{i}" for i in range(flat.shape[0]))  #为每个数值生成一个通道名，例如 "motion_0", "motion_1", ...。
        return flat, channel_names  #返回数值数组和通道名元组


class AvatarPipeline:  #这个类是核心协调者，它将所有后端整合起来，提供统一的接口来处理音频或文本输入，并返回面部表情、身体动作、情绪等结果
    def __init__(self) -> None:  #构造函数，在创建 AvatarPipeline 实例时自动调用。-> None 表示该方法不返回任何值
        self.face_engine = None  #用于存储 TensorRT 引擎对象（如果可用），初始为 None。优先使用 TensorRT 加速的面部表情模型
        self.face_onnx = OnnxAudioBackend(DEFAULT_FACE_ONNX, "blendshape")  #创建一个 OnnxAudioBackend 实例，用于加载 ONNX 格式的面部表情模型。如果 ONNX 文件存在且能成功加载，self.face_onnx.enabled 将为 True；否则为 False
                                                                                        #DEFAULT_FACE_ONNX：ONNX 模型路径（之前定义为 HOME / "Audio2Face-3D-v3.0" / "network.onnx"）
                                                                                        #"blendshape"：备用的输出名称
        self.emotion_onnx = OnnxAudioBackend(DEFAULT_EMOTION_ONNX, "emotion")  #类似地，创建情绪识别的 ONNX 后端
        self.chattts = ChatTTSBackend()  #创建 ChatTTS 文本转语音后端实例。如果 ChatTTS 可用，self.chattts.enabled 为 True
        self.llm = LLMBackend()  #创建 LLMBackend 实例。
        self.panto = PantoMatrixBackend()  #创建身体动作（PantoMatrix）后端实例。如果模型加载成功，self.panto.enabled 为 True

        if DEFAULT_FACE_ENGINE.exists():  #检查 TensorRT 引擎文件是否存在（DEFAULT_FACE_ENGINE 是 PROJECT_ROOT / "models" / "model.engine"）。如果存在，则尝试加载它
            try:  #创建一个 TRTInfer 实例，传入引擎文件路径（转为字符串）。这需要 trt_infer.py 中定义的 TRTInfer 类。
                self.face_engine = TRTInfer(str(DEFAULT_FACE_ENGINE))
                print(f"[Server] TensorRT face engine loaded: {DEFAULT_FACE_ENGINE}")
            except Exception as exc:  #如果加载失败（例如引擎文件损坏、GPU 不可用等），捕获异常并打印禁用信息，self.face_engine 保持为 None
                print(f"[Server] TensorRT face engine disabled: {exc}")

    def _infer_face(self, audio_16k: np.ndarray) -> Tuple[Tuple[str, ...], Tuple[float, ...]]:  #根据音频（16kHz）推理面部表情 blendshape 权重，返回名称元组和权重元组
                                                                                                #参数：audio_16k: np.ndarray，16kHz 采样率的音频波形
                                                                                                #返回：Tuple[Tuple[str, ...], Tuple[float, ...]]，两个元组：第一个是 ARKit 名称列表（固定 52 个），第二个是对应的权重值（0~1）
        if self.face_engine is not None:  #优先使用 TensorRT 引擎
            feat = make_mel_features(audio_16k)  #提取梅尔频谱特征，形状 (1, T, 128)
            out = self.face_engine.infer(feat).reshape(-1).astype(np.float32)  #调用 TensorRT 推理，结果形状为 (1, 52)，使用 reshape(-1) 展平为 (52,)，并转为 float32
        elif self.face_onnx.enabled:  #如果 TensorRT 不可用但 ONNX 后端可用
            out = self.face_onnx.infer(audio_16k).reshape(-1).astype(np.float32)  #直接传入原始音频（ONNX 内部会自己提取特征），同样展平为 52 维。
        else:  #如果两者都不可用，输出全零的 52 维数组
            out = np.zeros((52,), dtype=np.float32)

        if out.size < 52:  #如果输出数组大小不足 52（例如某些模型可能输出 51 维），则用 np.pad 在末尾补零，直到长度为 52。(0, 52 - out.size) 表示左边补 0 个，右边补所需个数
            out = np.pad(out, (0, 52 - out.size))
        out = out[:52]  #如果输出长度超过 52，则截断前 52 个
        out = np.clip(out, 0.0, 1.0)  #将所有值限制在 [0, 1] 之间（blendshape 权重通常在此范围）。

        return tuple(ARKit_52_NAMES), tuple(float(x) for x in out)  #tuple(ARKit_52_NAMES)：将全局的 ARKit 名称列表转为元组（不可变）
                                                                    #tuple(float(x) for x in out)：将每个权重转为 Python 浮点数并组成元组

    def _infer_emotion(self, audio_16k: np.ndarray) -> str:  #根据音频（16kHz）识别情绪，返回情绪标签字符串
        if not self.emotion_onnx.enabled:  #如果情绪 ONNX 后端未启用，直接返回 "neutral"
            return "neutral"

        try:  #尝试推理
            logits = self.emotion_onnx.infer(audio_16k).reshape(-1)  #调用 ONNX 推理，输出可能是形状 (1, num_classes)，使用 reshape(-1) 展平为一维数组。
            if logits.size == 0:  #如果输出为空，返回 "neutral"
                return "neutral"
            labels = ("neutral", "happy", "sad", "angry", "surprised", "fear", "disgust", "contempt")  #定义情绪标签列表，顺序与模型输出类别索引对应（常见的 8 种情绪）
            idx = int(np.argmax(logits))  #找到最大值的索引（预测的类别）
            if idx < len(labels):  #如果索引在标签范围内，返回对应的标签；否则返回 f"class_{idx}" 作为后备。
                return labels[idx]
            return f"class_{idx}"
        except Exception:  #如果任何步骤出错，返回 "neutral"
            return "neutral"

    def _infer_body(self, audio_24k: np.ndarray) -> Tuple[Tuple[str, ...], Tuple[float, ...]]:  #根据音频（24kHz）推理身体动作数据，返回通道名称元组和数值元组
        if not self.panto.enabled:  #如果 PantoMatrix 后端未启用，返回空元组 (), ()
            return (), ()
        try:
            values, channel_names = self.panto.infer(audio_24k)  #调用后端的 infer 方法，返回一维数组 values 和对应的通道名元组。
            return channel_names, tuple(float(x) for x in values)  #将 values 转为 Python 浮点数元组，与通道名元组一起返回。
        except Exception as exc:  #捕获异常，打印错误信息，返回空元组
            print(f"[Server] PantoMatrix inference failed: {exc}")
            return (), ()

    def infer_from_audio(self, audio_bytes: bytes) -> InferenceOutputs:  #从音频字节数据（WAV 格式）进行推理，返回 InferenceOutputs 数据类实例。注意：此方法不返回合成音频（wav 字段为空），因为输入已经是音频
                                                                         #参数：audio_bytes: bytes，WAV 文件的完整字节
                                                                         #返回：InferenceOutputs 对象
        audio_16k = decode_wav_bytes(audio_bytes, target_sr=16000)  #将 WAV 字节解码为 16kHz 的单声道浮点数组。
        audio_24k = decode_wav_bytes(audio_bytes, target_sr=24000)  #同样解码为 24kHz 的数组（用于身体动作模型）。

        face_names, face_weights = self._infer_face(audio_16k)  #获取面部表情结果
        body_names, body_values = self._infer_body(audio_24k)  #获取身体动作结果
        emotion = self._infer_emotion(audio_16k)  #获取情绪标签

        return InferenceOutputs(  #构造 InferenceOutputs 对象
            wav=b"",  #因为输入是音频，不需要返回合成语音，所以留空
            face_names=face_names,
            face_weights=face_weights,
            body_channel_names=body_names,
            body_values=body_values,
            emotion=emotion,
            status="ok",  #表示成功
        )

    def infer_from_text(self, text: str) -> InferenceOutputs:  #从文本输入进行推理：先使用 TTS 合成音频，再基于合成音频推理表情、动作和情绪，并返回合成音频本身
        prompt = (text or "").strip()  #将输入文本作为 prompt，调用 LLM 生成回复。如果 prompt 为空或生成失败，则回退到原文本
        reply_text = self.llm.generate(prompt) if prompt else ""
        if not reply_text:
            reply_text = prompt

        if self.chattts.enabled:  #如果 TTS 后端可用
            wav_np, sr = self.chattts.synthesize(reply_text)  #调用 TTS，得到音频数组和采样率（24000）
            wav_np = np.asarray(wav_np, dtype=np.float32).reshape(-1)  #确保是一维 float32 数组
            wav_bytes = pcm_to_wav_bytes(wav_np, sr)  #将 PCM 数组打包成 WAV 字节
        else:  #如果 TTS 不可用，wav_bytes = b"" 空字节
            wav_bytes = b""

        if wav_bytes:  #如果成功合成了音频
            audio_16k = decode_wav_bytes(wav_bytes, target_sr=16000)  #解码为 16kHz
            audio_24k = decode_wav_bytes(wav_bytes, target_sr=24000)  #解码为 24kHz
        else:  #如果没有音频（TTS 不可用），创建长度为 0 的空数组（避免后续处理出错）
            audio_16k = np.zeros((0,), dtype=np.float32)
            audio_24k = np.zeros((0,), dtype=np.float32)

        if audio_16k.size == 0:  #没有有效音频
            face_names, face_weights = tuple(ARKit_52_NAMES), tuple(0.0 for _ in range(52))  #面部名称照常，权重全 0。
            body_names, body_values = (), ()  #身体数据空
            emotion = "neutral"  #情绪中性
        else:  #有音频，正常推理（与 infer_from_audio 类似）
            face_names, face_weights = self._infer_face(audio_16k)
            body_names, body_values = self._infer_body(audio_24k)
            emotion = self._infer_emotion(audio_16k)

        preview = " ".join(reply_text.split())[:160]  #将回复文本的前 160 个字符（去除多余空白后）附加到状态字符串中，方便调试
        status = "ok" if not preview else f"ok | reply={preview}"

        return InferenceOutputs(
            wav=wav_bytes,  #返回合成音频（如果 TTS 成功）
            face_names=face_names,
            face_weights=face_weights,
            body_channel_names=body_names,
            body_values=body_values,
            emotion=emotion,
            status=status,
            reply_text=reply_text,  #返回时带上 reply_text
        )


PIPELINE = AvatarPipeline()  #创建一个 AvatarPipeline 类的实例，并赋值给全局变量 PIPELINE
                             #为什么是全局的：因为 gRPC 服务器需要处理多个客户端请求，所有请求共享同一个 pipeline 实例。这样可以避免重复加载模型（模型只加载一次），节省内存和时间


class AvatarServicer(pb_grpc.AvatarServiceServicer):  #定义了一个名为 AvatarServicer 的类，它继承自 pb_grpc.AvatarServiceServicer
                                                      #pb_grpc.AvatarServiceServicer 是由 avatar_stream.proto 编译自动生成的基类，里面定义了 StreamInfer 方法的框架（空实现）。我们在子类中重写这个方法，填入具体的业务逻辑
    def StreamInfer(self, request_iterator, context):  #方法名：StreamInfer，与 proto 文件中定义的 rpc 方法名完全一致
                                                       #request_iterator：一个迭代器，因为客户端可以流式发送多个 StreamRequest 消息。服务器通过 for req in request_iterator: 逐个接收请求
                                                       #context：gRPC 上下文对象，包含本次调用的元数据（如超时、认证信息等），本例中没有直接使用，但参数必须保留
                                                       #返回值：需要返回一个可迭代对象（通常是生成器），用于流式发送 StreamResponse 消息
        for req in request_iterator:  #遍历客户端发来的每一个 StreamRequest 消息，每次循环处理一个请求。因为支持双向流，服务器可以在处理每个请求后立即返回响应，不必等所有请求收完。
            try:  #从当前请求对象中提取两个字段
                seq = req.seq  #序列号（整数），用于匹配请求和响应（客户端可以根据 seq 知道这个响应是针对哪个请求的）
                mode = req.mode  #请求模式，枚举值（REQUEST_MODE_AUDIO、REQUEST_MODE_TEXT、REQUEST_MODE_MIC 或 REQUEST_MODE_UNSPECIFIED）

                if mode == pb.REQUEST_MODE_TEXT or (mode == pb.REQUEST_MODE_UNSPECIFIED and req.text):  #判断是否为文本模式
                                                                                                        #条件1：mode == pb.REQUEST_MODE_TEXT（明确指定文本模式）
                                                                                                        #条件2：mode == pb.REQUEST_MODE_UNSPECIFIED and req.text（模式未指定，但请求中包含了 text 字段，说明客户端想用文本输入）
                    outputs = PIPELINE.infer_from_text(req.text)  #如果满足条件，调用 PIPELINE.infer_from_text(req.text) 进行文本驱动，返回的 outputs 是一个 InferenceOutputs 对象

                elif mode in (pb.REQUEST_MODE_AUDIO, pb.REQUEST_MODE_MIC) or req.wav:  #否则判断是否为音频模式
                                                                                       #条件：mode 是 REQUEST_MODE_AUDIO 或 REQUEST_MODE_MIC，或者请求中包含了 wav 字段
                    outputs = PIPELINE.infer_from_audio(req.wav)  #满足条件则调用 PIPELINE.infer_from_audio(req.wav) 进行音频驱动

                else:  #如果以上条件都不满足（例如 mode 是 UNSPECIFIED 且既没有 text 也没有 wav）
                    outputs = InferenceOutputs(status="unsupported request mode")  #创建一个 InferenceOutputs 对象，状态设为 "unsupported request mode"，其他字段为默认值

                if outputs.reply_text:  #在处理文本请求时，如果返回的 reply_text 非空，打印到服务器控制台，便于观察 LLM 生成的回复
                    print(f"[Server] seq={seq} reply={outputs.reply_text}")

                yield pb.StreamResponse(  #构造并返回响应消息：使用 yield 返回一个 StreamResponse 对象
                    seq=seq,  #原样返回请求的序列号
                    wav=outputs.wav if req.want_audio else b"",  #只有当客户端在请求中设置 want_audio=True 时，才返回合成音频（文本模式会生成，音频模式返回空字节串）。否则返回空字节
                    face=pb.FaceBlendShape(  #如果客户端 want_face=True，则将 outputs.face_names 和 outputs.face_weights 转换为列表填入；否则填入空列表
                        names=list(outputs.face_names) if req.want_face else [],
                        weights=list(outputs.face_weights) if req.want_face else [],
                    ),
                    body=pb.BodyMotion(  #如果客户端 want_body=True，则填入身体通道名、数值，并设置 format 为 "axis_angle_flat"；否则给空列表和空字符串
                        channel_names=list(outputs.body_channel_names) if req.want_body else [],
                        values=list(outputs.body_values) if req.want_body else [],
                        format="axis_angle_flat" if req.want_body else "",  #
                    ),
                    emotion=outputs.emotion,  #直接填入情绪标签
                    status=outputs.status,  #填入状态（正常为 "ok"，出错时为错误信息）
                )

            except Exception as exc:  #如果在处理某个请求时发生任何异常（例如模型推理失败、音频解码错误、文件不存在等），则捕获异常，打印错误日志，并返回一个响应，其中 status 字段为异常信息字符串
                print(f"[Server] seq={getattr(req, 'seq', -1)} error: {exc}")  #getattr(req, 'seq', -1) 尝试从请求对象中获取 seq 属性，如果获取失败（比如请求对象本身无效），则使用默认值 -1。这样即使出错也能返回一个合理的序列号
                yield pb.StreamResponse(seq=getattr(req, 'seq', -1), status=str(exc))

def serve():  #创建并启动 gRPC 服务器
    server = grpc.server(  #创建一个 gRPC 服务器对象
        futures.ThreadPoolExecutor(max_workers=4),  #使用线程池，最多允许 4 个线程 同时处理请求。这意味着服务器可以并发处理 4 个客户端连接（或同一个连接中的多个并发流）
        options=[  #设置服务器选项
            ("grpc.max_send_message_length", 200 * 1024 * 1024),  #最大发送消息长度设为 200 MB（默认是 4 MB）。因为响应中可能包含较长的合成音频（WAV），需要调大限制
            ("grpc.max_receive_message_length", 200 * 1024 * 1024),  #最大接收消息长度同样设为 200 MB，用于接收客户端上传的长音频文件或大文本
        ],
    )
    pb_grpc.add_AvatarServiceServicer_to_server(AvatarServicer(), server)  #将我们实现的 AvatarServicer 实例注册到 gRPC 服务器上。这样服务器就知道：当收到 AvatarService 的 RPC 调用时，应该交给这个 servicer 对象的 StreamInfer 方法去处理
    server.add_insecure_port("[::]:50051")  #添加一个不加密的监听端口。生产环境通常需要使用加密端口（add_secure_port），这里为了简化使用非加密方式
                                            #"[::]" 表示监听所有可用的网络接口（IPv6 和 IPv4 都支持，等同于 0.0.0.0）
                                            #50051 是端口号（gRPC 常用的默认端口）
    server.start()  #启动服务器。该调用是非阻塞的，服务器会在后台线程中运行，继续执行后面的代码
    print("Server started on [::]:50051")  #在控制台打印启动成功信息，方便运维人员确认
    server.wait_for_termination()  #阻塞当前线程（主线程），直到服务器被关闭（例如按 Ctrl+C 或收到终止信号）。如果没有这行，程序会立即退出


if __name__ == "__main__":
    serve()
