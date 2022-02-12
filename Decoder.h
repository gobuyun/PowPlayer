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
	void outputError(std::string&& module, int ret); // ��ӡ����

	void openAudioStream(); // ����Ƶ��
	void openVideoStream(); // ����Ƶ��
	bool openStream(std::string filePath); // �����ϱ���������
	void closeVideoStream();
	void closeAudioStream();
	void closeStream();

	void videoSyncClock(int lastPts); // ��Ƶͬ��
	void refreshVideo(); // ������Ƶ֡
	void updatePlayControlState(); // ����playcontrol����ر��
	void updateAudioBuffer(); // ������Ƶbuffer

	bool playAudioEof(); // �ж���Ƶ�Ƿ񲥷����
	bool playVideoEof(); // �ж���Ƶ�Ƿ񲥷����

	void evalCacheMax(); // ��������Ƶcache preload�����ֵ

	bool isVideoCacheOverLoad(); // �ж���Ƶ�Ƿ����
	bool isAudioCacheOverLoad(); // �ж���Ƶ�Ƿ����

	static int eventLoop(void* data); // �¼�ѭ��
	static int audioDecodeThread(void* data); // ��Ƶ����
	static int videoDecodeThread(void* data); // ��Ƶ����
	static int readThread(void* data); // ��ȡ
	static void audioCallback(void* userdata, Uint8* stream, int len); 

signals:
	void newVideoFrame(const QVideoFrame& frame); // �µ���Ƶ֡
	void videoUrlChanged();
	void dataReady(); // ��ʼ�����
	void playFinished(); // �������
public slots:
	void onNewVideoFrameReceived(const QVideoFrame& frame); // ��Ⱦ��Ƶ����

private:
	enum class QueueState
	{
		NORMAL = 0,
		EMPTY,
		FULL,
		LAST,
	};
	struct PacketQueue // AVPacket����
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

	struct VideoData // �������Ƶ���ݽṹ��
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
	struct VideoFrameQueue // �������Ƶ���ݶ���
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

	struct AudioData // �������Ƶ����
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
	struct AudioFrameQueue // �������Ƶ���ݶ���
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

	struct PlayControlState // ���ſ��Ƶ����״̬
	{
		PlayControlState()
		{
			seekMutex = SDL_CreateMutex();
		}
		~PlayControlState()
		{
			SDL_DestroyMutex(seekMutex);
		}
		bool bReadEof = false; // ��ȡ�����
		bool bAudioDecodeEof = false; // ��Ƶ�������
		bool bVideoDecodeEof = false; // ��Ƶ�������
		bool bPlayAudioEof = false; // ������Ƶ���
		bool bPlayVideoEof = false; // ������Ƶ���
		bool bPlayEof = false; // �������

		bool bAbort = false; // ��ֹ���
		bool bPause = false;
		bool bAutoStart = false;
		float speed = 1.0; // ��������

		int64_t seekingTime = -1;
		SDL_mutex* seekMutex; // seek��

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

	// ������
	PacketQueue m_audioPktQue;
	PacketQueue m_videoPktQue;
	// ֡����
	AudioFrameQueue m_audioFrameQue;
	VideoFrameQueue m_videoFrameQue;

	// audio ���
	void* m_audioBuff = nullptr; // �ز�����buffer
	int m_audioBufferCurInx = -1; // ����sdlcallback
	int m_audioBufferSize = 0; // ����sdlcallback
	AudioData* m_curAudioData = nullptr; // ����sdlcallback
	int m_audioBufferTotalSize = 0;
	int m_nAudioInx = -1; // ��Ƶ������
	int m_nChannelFormatByte; // ��Ƶchannel*format
	int64_t m_audioClk = 0; // ��Ƶʱ��
	AVCodecContext* m_audioCodecCtx = nullptr;
	SwrContext* m_swrCtx = nullptr;
	AVCodecParameters* m_pAudioCodecParam = nullptr;// ��Ƶ����
	SDL_AudioSpec m_settingSpec;
	int m_audioFormatPreset[2] = { AV_SAMPLE_FMT_S16, AUDIO_S16SYS };
	QAudioFormat m_audioFormat;

	// video ���
	void* m_pVideoOutBuffer;
	int m_videoOutBufferSize;
	int m_videoDisplayDelay = 0;
	int m_nVideoInx = -1; // ��Ƶ������
	int m_nOutputBufferSize = 0;
	int64_t m_videoClk = 0; // ��Ƶʱ��
	AVCodecContext* m_videoCodecCtx = nullptr;
	struct SwsContext* m_swsCtx = nullptr;
	AVCodecParameters* m_pVideoCodecParam = nullptr; // ��Ƶ����
	SDL_Window* m_pVideowin;
	SDL_Renderer* m_pRender;
	SDL_Texture* m_pTexture;
	SDL_Rect m_textureRect;
	//int preset[2] = { AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2 };
	int preset[2] = { AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_YUY2 };
	VideoData* m_curVideoData = nullptr;
	VideoData* m_lastVideoData = nullptr;
	std::shared_ptr<QVideoFrame> m_frame = nullptr;

	SDL_Thread* m_audioDecThread; // ��Ƶ�����߳�
	SDL_Thread* m_videoDecThread; // ��Ƶ�����߳�
	SDL_Thread* m_readThread; // ��ȡ�߳�
	SDL_Thread* m_eventLoopThread; // ��ȡ�߳�

	// ״̬����
	PlayControlState m_playControl;

	std::string m_filePath; // ý��·��

	bool m_bInitSuccessful = false; // ��ʼ���ɹ�

	const int PRELOADSEC = 1; // Ԥ����3������
	int m_videoCacheMaxByte = 0;
	int m_audioCacheMaxByte = 0;
};
