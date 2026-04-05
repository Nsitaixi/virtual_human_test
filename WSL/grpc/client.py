#这个文件实现了一个 gRPC 客户端，用于与服务器（server.py）进行通信。客户端可以向服务器发送音频文件或文本，并接收服务器返回的面部表情、身体动作、情绪以及合成语音等数据。

from __future__ import annotations  #这是Python的一个“未来特性”导入，主要作用是允许在类型注解中使用尚未定义的类

import argparse  #命令行参数解析模块，用于让用户通过命令行指定输入文件、服务器地址等。
import io  #提供字节流操作（io.BytesIO），用于在内存中模拟文件。
import time  #提供 time.sleep，用于控制请求发送的间隔。
import wave  #Python 标准库中的 WAV 文件读写模块，用于将 PCM 数据打包成 WAV 格式。
from pathlib import Path  #现代化的路径处理模块，让文件路径操作更简洁安全。

import grpc  #gRPC 库，用于创建客户端通道和调用远程服务。

import avatar_stream_pb2 as pb  #导入自动生成的消息定义模块，包含 StreamRequest、StreamResponse 等类。
import avatar_stream_pb2_grpc as pb_grpc  #导入自动生成的服务定义模块，包含 AvatarServiceStub 客户端存根类。

DEFAULT_WAV = Path("/mnt/d/Nsitaixi/Unreal/UnrealProjects/virtual_human_test/Assets/sample.wav")  #定义默认 WAV 文件路径。如果用户在命令行没有指定 --wav 参数，就会使用这个默认文件。实际使用时需要根据你的环境修改或通过 --wav 参数指定。


def build_wav_bytes(pcm_frames: bytes, sample_rate: int, channels: int, sampwidth: int) -> bytes:  #将 PCM 裸数据（字节串）打包成完整的 WAV 文件字节。WAV 文件包含头部信息（声道数、采样宽度、采样率等）和数据块。
                                                                                                   #pcm_frames: bytes：PCM 音频数据（通常是多帧的原始字节）。
                                                                                                   #sample_rate: int：采样率（Hz），例如 16000。
                                                                                                   #channels: int：声道数（1 为单声道，2 为立体声）。
                                                                                                   #sampwidth: int：采样宽度（字节数），例如 2 表示 16 位深度。
    buf = io.BytesIO()  #创建一个内存中的字节缓冲区，可以像文件一样写入。
    with wave.open(buf, "wb") as w:  #用 wave 模块以写二进制模式打开这个缓冲区，得到一个 wave.Wave_write 对象 w。使用 with 确保最后自动关闭。
        w.setnchannels(channels)  #设置 WAV 头中的声道数。
        w.setsampwidth(sampwidth)  #设置采样宽度（字节数）。
        w.setframerate(sample_rate)  #设置采样率。
        w.writeframes(pcm_frames)  #写入 PCM 数据。
    return buf.getvalue()  #获取缓冲区中完整的 WAV 文件字节数据并返回。


def gen_audio_requests(wav_path: Path):  #这是一个生成器函数（使用了 yield），用于将 WAV 文件分块读取，并为每个块生成一个 StreamRequest 消息。这样客户端可以一边读取文件一边发送，实现流式上传。
    if not wav_path.exists():  #检查文件是否存在，不存在则抛出异常。
        raise FileNotFoundError(f"WAV not found: {wav_path}")

    with wave.open(str(wav_path), "rb") as wf:  #以只读二进制模式打开 WAV 文件，得到 wave.Wave_read 对象 wf。
        sample_rate = wf.getframerate()  #获取 WAV 文件的采样率。
        channels = wf.getnchannels()  #获取声道数。
        sampwidth = wf.getsampwidth()  #获取采样宽度（字节数）。
        chunk_seconds = 0.5  #每个音频块的长度（秒），这里设为 0.5 秒。服务器每收到一个块就会开始推理，可以实现低延迟响应。
        chunk_frames = int(sample_rate * chunk_seconds)  #根据采样率计算每个块包含多少帧（样本点数）。
        seq = 0  #初始化序列号，从 0 开始。

        while True:  #循环读取文件直到结束
            frames = wf.readframes(chunk_frames)  #读取 chunk_frames 个样本帧（注意：帧数是样本点数，不是字节数）。返回 bytes 对象。
            if not frames:  #如果读取不到数据（文件末尾），则跳出循环。
                break

            yield pb.StreamRequest(  #产生一个 StreamRequest 消息。
                seq=seq,  #当前序列号。
                mode=pb.REQUEST_MODE_AUDIO,  #指定为音频模式。
                wav=build_wav_bytes(frames, sample_rate, channels, sampwidth),  #将这一小段 PCM 数据打包成 WAV 字节。
                want_audio=False,  #客户端不需要服务器返回合成音频（因为输入已经是音频）。
                want_face=True,  #需要面部表情数据。
                want_body=True,  #需要身体动作数据。
            )
            seq += 1  #序列号自增。
            time.sleep(chunk_seconds * 0.8)  #睡眠 0.4 秒（0.5 * 0.8）。这样做是为了模拟实时流，避免发送速度过快导致服务器缓冲区积压。实际应用中可以根据网络和服务器处理能力调整。


def gen_text_request(text: str):  #生成一个只包含一个请求的生成器，用于文本输入模式。
    yield pb.StreamRequest(  #产生一个 StreamRequest 消息。
        seq=0,  #序列号固定为 0，因为只有一个请求。
        mode=pb.REQUEST_MODE_TEXT,  #指定为文本模式。
        text=text,  #传入要合成的文本。
        want_audio=True,  #需要服务器返回合成语音（TTS 生成）。
        want_face=True,  #需要面部表情。
        want_body=True,  #需要身体动作。
    )


def run_client_audio(wav_path: Path, host: str, port: int):  #连接 gRPC 服务器，发送音频分块请求，并打印服务器返回的响应摘要。
    channel = grpc.insecure_channel(f"{host}:{port}")  #创建不加密的 gRPC 通道，连接到指定的主机和端口。
    stub = pb_grpc.AvatarServiceStub(channel)  #创建服务存根（stub），通过它可以调用远程方法。
    responses = stub.StreamInfer(gen_audio_requests(wav_path))  #调用 StreamInfer 方法，传入音频请求生成器。该方法返回一个响应迭代器（因为服务器也是流式返回）。
    for resp in responses:  #遍历每个响应。
        print(f"[Client-Audio] seq={resp.seq} status={resp.status}")  #打印序列号和状态。
        print(f"  emotion={resp.emotion}")  #打印情绪。
        print(f"  face={len(resp.face.weights)} weights, names={len(resp.face.names)}")  #打印面部权重数量和名称数量（通常都是 52）。
        print(f"  body={len(resp.body.values)} values, format={resp.body.format}")  #打印身体数值数量和格式。


def run_client_text(text: str, host: str, port: int):  #连接服务器，发送文本请求，并打印响应信息。与音频客户端类似，但额外打印了返回的合成音频字节长度。
    channel = grpc.insecure_channel(f"{host}:{port}")
    stub = pb_grpc.AvatarServiceStub(channel)
    responses = stub.StreamInfer(gen_text_request(text))
    for resp in responses:
        print(f"[Client-Text] seq={resp.seq} status={resp.status}")
        print(f"  emotion={resp.emotion}")
        print(f"  wav_bytes={len(resp.wav)}")
        print(f"  face={len(resp.face.weights)} weights, names={len(resp.face.names)}")
        print(f"  body={len(resp.body.values)} values, format={resp.body.format}")  #


def main():  #
    parser = argparse.ArgumentParser()  #创建命令行参数解析器。
    parser.add_argument("--wav", type=Path, default=DEFAULT_WAV, help="WAV 文件路径")  #WAV 文件路径，类型为 Path，默认值为 DEFAULT_WAV，帮助说明。
    parser.add_argument("--text", type=str, default="", help="文本输入，启用 TTS + V2F + V2M")  #文本字符串，默认空字符串。如果提供了文本，则运行文本模式；否则运行音频模式。
    parser.add_argument("--host", type=str, default="127.0.0.1", help="gRPC server 地址")  #服务器地址，默认 127.0.0.1。
    parser.add_argument("--port", type=int, default=50051, help="gRPC server 端口")  #服务器端口，默认 50051。
    args = parser.parse_args()  #解析命令行参数。

    if args.text:  #如果 --text 参数非空，调用 run_client_text；否则调用 run_client_audio 并传入 WAV 路径。
        run_client_text(args.text, args.host, args.port)
    else:
        run_client_audio(args.wav, args.host, args.port)


if __name__ == "__main__":
    main()