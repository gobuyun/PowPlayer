# PowPlayer
Qt Quick + ffmpeg = PowPlayer

项目原先使用SDL渲染音视频，之后改用qml，看到网上案例相对较少，于是上传分享

但项目本意是做一款自己的播放器，同时也能促进自己进步。

后续规划功能支持： 
- 1、播放器基本功能：进度条、seek、暂停/播放、快进快退、音量、音视频倍数、最大最小化
- 2、摄像头/麦克风采集
- 3、网络推拉流：类似vlc，可rtmp推流、可rtsp拉流
- 4、滤镜、特效：AVFilter相关，这个最后才支持
- 5、皮肤功能

如何跑起来： 
- 1、配置好Qt5开发环境，最好Qt5.13.X
- 2、VS安装Qt插件，网上可找资料，步骤很简单
- 3、打开sln，配置工程属性，包含ffmpegSDK/include头文件，链接ffmpeg相关lib
- 4、编译
- 5、程序依赖ffmpeg的dll，把ffmpegSDK/bin的dll全部拷贝过去运行目录下即可
PS: 自行修改main.qml的videoUrl属性

2022/2/12
仅能播放音视频，无ui
实测在Qt5.9.9、Qt5.13.x均能运行
