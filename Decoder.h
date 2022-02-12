#pragma once

#include <string>
#include <queue>
#include <memory>

#include <QObject>
#include <QAbstractVideoSurface>
#include <QVideoFrame>
#include <QVideoSurfaceFormat>
#include <QIODevice>
#include <QAudioFormat>

extern "C"
{
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "libswscale/swscale.h"
#include "libavdevice/avdevice.h"
#include "libswresample/swresample.h"
#include "SDL2/SDL.h"
#include <SDL2/SDL_thread.h>
}


class Decoder : public QIODevice
{
	Q_OBJECT
	Q_PROPERTY(QAbstractVideoSurface* videoSurface READ videoSurface WRITE setVideoSurface)
	Q_PROPERTY(QString videoUrl READ videoUrl WRITE setVideoUrl NOTIFY videoUrlChanged)

public:
	QAbstractVideoSurface* videoSurface() { return m_videoSurface; }
	void setVideoSurface(QAbstractVideoSurface* surface);

	QString videoUrl() { return m_videoUrl; }
	void setVideoUrl(QString videoUrl);

	void setFormat(int width, int heigth, QVideoFrame::PixelFormat pixFormat);
private:
	QAbstractVideoSurface* m_videoSurface = nullptr;
	QVideoSurfaceFormat  m_surfaceFmt;
	QString m_videoUrl;


public:
	Decoder(std::string&& fullPath, QObject* parents = nullptr);
	Decoder(QObject* parents = nullptr);
	~Decoder();

	QAudioFormat* getAudioFormat() {
		return &m_audioFormat; 
	}

	virtual qint64 readData(char* steam, qint64 len);
	virtual qint64 writeData(const char* data, qint64 len) {
		Q_UNUSED(data);
		Q_UNUSED(len);
		return 0;
	}

private:
	void outputError(std::string&& module, int ret); // 打印错误

	void openAudioStream(); // 打开音频流
	void openVideoStream(); // 打开视频流
	bool openStream(std::string filePath); // 调用上边两个函数
	void closeVideoStream();
	void closeAudioStream();
	void closeStream();

	void videoSyncClock(int lastPts); // 视频同步
	void refreshVideo(); // 更新视频帧
	void updatePlayControlState(); // 更新playcontrol的相关标记
	void updateAudioBuffer(); // 更新音频buffer

	bool playAudioEof(); // 判断音频是否播放完成
	bool playVideoEof(); // 判断视频是否播放完成

	void evalCacheMax(); // 评估音视频cache preload的最大值

	bool isVideoCacheOverLoad(); // 判断视频是否过载
	bool isAudioCacheOverLoad(); // 判断音频是否过载

	static int eventLoop(void* data); // 事件循环
	static int audioDecodeThread(void* data); // 音频解码
	static int videoDecodeThread(void* data); // 视频解码
	static int readThread(void* data); // 读取
	static void audioCallback(void* userdata, Uint8* stream, int len); 

signals:
	void newVideoFrame(const QVideoFrame& frame); // 新的视频帧
	void videoUrlChanged();
	void dataReady(); // 初始化完成
	void playFinished(); // 播放完成
public slots:
	void onNewVideoFrameReceived(const QVideoFrame& frame); // 渲染视频数据

private:
	enum class QueueState
	{
		NORMAL = 0,
		EMPTY,
		FULL,
		LAST,
	};
	struct PacketQueue // AVPacket队列
	{
		std::queue<AVPacket*> data;
		SDL_mutex* mutex = nullptr;
		PacketQueue()
		{
			mutex = SDL_CreateMutex();
		}
		~PacketQueue()
		{
			SDL_DestroyMutex(mutex);
		}
		QueueState push(AVPacket* input)
		{
			QueueState state = QueueState::NORMAL;
			SDL_LockMutex(mutex);
			do
			{
				AVPacket* pkt = nullptr;
				if (input)
				{
					pkt = av_packet_alloc();
					av_packet_unref(pkt);
					av_packet_move_ref(pkt, input);
				}
				data.push(pkt);
				//  
				// todo: FULL
				// state = QueueState::FULL;
				//
			} while (0);
			SDL_UnlockMutex(mutex);
			return state;
		}
		QueueState pop(AVPacket* output)
		{
			QueueState state = QueueState::NORMAL;
			AVPacket* front = nullptr;
			SDL_LockMutex(mutex);
			do 
			{
				if (data.size() == 0)
				{
					state = QueueState::EMPTY;
					break;
				}
				front = data.front();
				data.pop();
				if (!front)
				{
					state = QueueState::LAST;
				}
			} while (0);
			SDL_UnlockMutex(mutex);
			if (front)
			{
				av_packet_unref(output);
				av_packet_move_ref(output, front);
				av_packet_free(&front);
			}
			return state;
		}
	};

	struct VideoData // 解码后视频数据结构体
	{
		void* pVideoBuffer;
		int nBufferSize;
		int sdlRenderLinePixelNum;
		int framePts;
		VideoData(void* videoBuffer, int bufferSize, int linePixelNum, int pts)
			: nBufferSize(bufferSize)
			, sdlRenderLinePixelNum(linePixelNum)
			, framePts(pts)
		{
			pVideoBuffer = av_mallocz(bufferSize);
			memcpy(pVideoBuffer, videoBuffer, bufferSize);
		}
		~VideoData()
		{
			av_free(pVideoBuffer);
		}
	};
	struct VideoFrameQueue // 解码后视频数据队列
	{
		std::queue<VideoData*> data;
		unsigned long long totalDataByte = 0;
		SDL_mutex* mutex = nullptr;

		VideoFrameQueue()
		{
			mutex = SDL_CreateMutex();
		}
		~VideoFrameQueue()
		{
			SDL_DestroyMutex(mutex);
		}
		QueueState push(VideoData* input)
		{
			QueueState state = QueueState::NORMAL;
			
			SDL_LockMutex(mutex);
			do
			{
				data.push(input);
				totalDataByte += input->nBufferSize;
				//  
				// todo: FULL
				// state = QueueState::FULL;
				//
			} while (0);
			SDL_UnlockMutex(mutex);
			return state;
		}
		QueueState pop(VideoData** output)
		{
			QueueState state = QueueState::NORMAL;
			SDL_LockMutex(mutex);
			do
			{
				if (data.size() == 0)
				{
					state = QueueState::EMPTY;
					break;
				}
				*output = data.front();
				data.pop();
				totalDataByte -= (*output)->nBufferSize;
			} while (0);
			SDL_UnlockMutex(mutex);
			return state;
		}
		QueueState getCurState()
		{
			QueueState state = QueueState::NORMAL;
			SDL_LockMutex(mutex);
			if (data.size() == 0)
			{
				state = QueueState::EMPTY;
			}
			SDL_UnlockMutex(mutex);
			return state;
		}
	};

	struct AudioData // 解码后音频数据
	{
		void* pAudioBuffer;
		int nBufferSize;
		int framePts;
		AudioData(void* audioBuffer, int bufferSize, int pts)
			: nBufferSize(bufferSize)
			, framePts(pts)
		{
			pAudioBuffer = av_mallocz(bufferSize);
			memcpy(pAudioBuffer, audioBuffer, bufferSize);
		}
		~AudioData()
		{
			av_free(pAudioBuffer);
		}
	};
	struct AudioFrameQueue // 解码后音频数据队列
	{
		std::queue<AudioData*> data;
		unsigned long long totalDataByte = 0;
		SDL_mutex* mutex = nullptr;

		AudioFrameQueue()
		{
			mutex = SDL_CreateMutex();
		}
		~AudioFrameQueue()
		{
			SDL_DestroyMutex(mutex);
		}
		QueueState push(AudioData* input)
		{
			QueueState state = QueueState::NORMAL;

			SDL_LockMutex(mutex);
			do
			{
				data.push(input);
				totalDataByte += input->nBufferSize;
				//  
				// todo: FULL
				// state = QueueState::FULL;
				//
			} while (0);
			SDL_UnlockMutex(mutex);
			return state;
		}
		QueueState pop(AudioData** output)
		{
			QueueState state = QueueState::NORMAL;
			SDL_LockMutex(mutex);
			do
			{
				if (data.size() == 0)
				{
					state = QueueState::EMPTY;
					break;
				}
				*output = data.front();
				data.pop();
				totalDataByte -= (*output)->nBufferSize;
			} while (0);
			SDL_UnlockMutex(mutex);
			return state;
		}
		QueueState getCurState()
		{
			QueueState state = QueueState::NORMAL;
			SDL_LockMutex(mutex);
			if (data.size() == 0)
			{
				state = QueueState::EMPTY;
			}
			SDL_UnlockMutex(mutex);
			return state;
		}
	};

	struct PlayControlState // 播放控制的相关状态
	{
		PlayControlState()
		{
			seekMutex = SDL_CreateMutex();
		}
		~PlayControlState()
		{
			SDL_DestroyMutex(seekMutex);
		}
		bool bReadEof = false; // 读取包完成
		bool bAudioDecodeEof = false; // 音频解码完成
		bool bVideoDecodeEof = false; // 视频解码完成
		bool bPlayAudioEof = false; // 播放音频完成
		bool bPlayVideoEof = false; // 播放视频完成
		bool bPlayEof = false; // 播放完成

		bool bAbort = false; // 中止标记
		bool bPause = false;
		bool bAutoStart = false;
		float speed = 1.0; // 倍数播放

		int64_t seekingTime = -1;
		SDL_mutex* seekMutex; // seek锁

		void seek()
		{
			bReadEof = false;
			bAudioDecodeEof = false;
			bVideoDecodeEof = false;
			bPlayAudioEof = false;
			bPlayVideoEof = false;
			bPlayEof = false;
			bPause = false;
		}
	};

	AVFormatContext* m_fmtCtx = nullptr;

	AVPacket* m_pRreadPkt;
	AVFrame* m_pAudioFrame;
	AVFrame* m_pVideoFrame;
	AVFrame* m_pVideoOutFrame;

	// 包队列
	PacketQueue m_audioPktQue;
	PacketQueue m_videoPktQue;
	// 帧队列
	AudioFrameQueue m_audioFrameQue;
	VideoFrameQueue m_videoFrameQue;

	// audio 相关
	void* m_audioBuff = nullptr; // 重采样的buffer
	int m_audioBufferCurInx = -1; // 用于sdlcallback
	int m_audioBufferSize = 0; // 用于sdlcallback
	AudioData* m_curAudioData = nullptr; // 用于sdlcallback
	int m_audioBufferTotalSize = 0;
	int m_nAudioInx = -1; // 音频流索引
	int m_nChannelFormatByte; // 音频channel*format
	int64_t m_audioClk = 0; // 音频时钟
	AVCodecContext* m_audioCodecCtx = nullptr;
	SwrContext* m_swrCtx = nullptr;
	AVCodecParameters* m_pAudioCodecParam = nullptr;// 音频参数
	SDL_AudioSpec m_settingSpec;
	int m_audioFormatPreset[2] = { AV_SAMPLE_FMT_S16, AUDIO_S16SYS };
	QAudioFormat m_audioFormat;

	// video 相关
	void* m_pVideoOutBuffer;
	int m_videoOutBufferSize;
	int m_videoDisplayDelay = 0;
	int m_nVideoInx = -1; // 视频流索引
	int m_nOutputBufferSize = 0;
	int64_t m_videoClk = 0; // 视频时钟
	AVCodecContext* m_videoCodecCtx = nullptr;
	struct SwsContext* m_swsCtx = nullptr;
	AVCodecParameters* m_pVideoCodecParam = nullptr; // 视频参数
	SDL_Window* m_pVideowin;
	SDL_Renderer* m_pRender;
	SDL_Texture* m_pTexture;
	SDL_Rect m_textureRect;
	//int preset[2] = { AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2 };
	int preset[2] = { AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_YUY2 };
	VideoData* m_curVideoData = nullptr;
	VideoData* m_lastVideoData = nullptr;
	std::shared_ptr<QVideoFrame> m_frame = nullptr;

	SDL_Thread* m_audioDecThread; // 音频解码线程
	SDL_Thread* m_videoDecThread; // 视频解码线程
	SDL_Thread* m_readThread; // 读取线程
	SDL_Thread* m_eventLoopThread; // 读取线程

	// 状态控制
	PlayControlState m_playControl;

	std::string m_filePath; // 媒体路径

	bool m_bInitSuccessful = false; // 初始化成功

	const int PRELOADSEC = 1; // 预加载3秒数据
	int m_videoCacheMaxByte = 0;
	int m_audioCacheMaxByte = 0;
};
