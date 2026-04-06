# virtual_human_test
这是一个把 UE5 MetaHuman 和 WSL 端 AI 推理服务连起来的虚拟人项目。

它的核心流程是：
用户在 UE 里通过 文字输入、麦克风语音或 音频文件触发交互；UE 通过 gRPC 把请求发到 WSL；WSL 端负责做 文本理解 / 语音合成 / 脸部表情推理 / 身体动作推理；最后把结果返回 UE，再驱动 MetaHuman 的脸和身体动起来。

简单说，它实现的是一个可以“说话、响应、做表情、做动作”的实时虚拟人系统。

# 注意事项
系统内存 <= 32GB 可能无法支撑该插件在UE5.7项目中正常运行。

# 用到的模型

TTS:https://github.com/2noise/ChatTTS

V2F: https://huggingface.co/nvidia/Audio2Face-3D-3.0

     https://huggingface.co/nvidia/Audio2Emotion-v3.0

V2M: https://github.com/PantoMatrix/PantoMatrix.git
