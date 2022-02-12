#include "Decoder.h"

#include <iostream>

//#define SAVEPCM
#ifdef SAVEPCM
FILE* g_pcmFp;
#endif

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

Decoder::Decoder(std::string&& fullPath, QObject* parents) : QIODevice(parents)
{
	m_filePath = fullPath;
	m_bInitSuccessful = openStream(std::forward<std::string&&>(fullPath));
	eventLoop(this);
}

Decoder::Decoder(QObject* parents) : QIODevice(parents)
{
}

Decoder::~Decoder()
{
	closeStream();
}

void Decoder::setVideoUrl(QString videoUrl)
{
	if (m_videoUrl != videoUrl)
	{
		m_filePath = videoUrl.toStdString();
		m_videoUrl = videoUrl;
		emit videoUrlChanged();
		if (m_videoSurface)
		{
			m_bInitSuccessful = openStream(m_videoUrl.toStdString());
		}
	}
}

void Decoder::setVideoSurface(QAbstractVideoSurface* surface)
{
	if (m_videoSurface && m_videoSurface != surface && m_videoSurface->isActive()) {
		m_videoSurface->stop();
	}
	
	m_videoSurface = surface;

	if (m_videoSurface && m_surfaceFmt.isValid())
	{
		m_surfaceFmt = m_videoSurface->nearestFormat(m_surfaceFmt);
		m_videoSurface->start(m_surfaceFmt);
	}

	if (!m_videoUrl.isEmpty())
	{
		m_bInitSuccessful = openStream(m_videoUrl.toStdString());
	}
}

void Decoder::setFormat(int width, int heigth, QVideoFrame::PixelFormat pixFormat)
{
	QVideoSurfaceFormat format(QSize(width, heigth), pixFormat);
	m_surfaceFmt = format;

	if (m_videoSurface) 
	{
		if (m_videoSurface->isActive()) 
		{
			m_videoSurface->stop();
		}
		m_surfaceFmt = m_videoSurface->nearestFormat(format);
		m_videoSurface->start(m_surfaceFmt);
	}
}

void Decoder::onNewVideoFrameReceived(const QVideoFrame& frame)
{
	if (m_videoSurface)
		m_videoSurface->present(frame);
}

void Decoder::openVideoStream()
{
	AVCodec* pVideoCdec = avcodec_find_decoder(m_pVideoCodecParam->codec_id);
	if (pVideoCdec)
	{
		m_videoCodecCtx = avcodec_alloc_context3(pVideoCdec);
		avcodec_parameters_to_context(m_videoCodecCtx, m_pVideoCodecParam);
		avcodec_open2(m_videoCodecCtx, pVideoCdec, nullptr);
		m_swsCtx = sws_getContext(m_pVideoCodecParam->width,
			m_pVideoCodecParam->height,
			(AVPixelFormat)m_pVideoCodecParam->format,
			m_pVideoCodecParam->width,
			m_pVideoCodecParam->height,
			(AVPixelFormat)preset[0], 0, nullptr, nullptr, nullptr);

		m_pVideoFrame = av_frame_alloc();
		m_pVideoOutFrame = av_frame_alloc();
		m_videoOutBufferSize = av_image_get_buffer_size((AVPixelFormat)preset[0], m_pVideoCodecParam->width, m_pVideoCodecParam->height, 1);
		m_pVideoOutBuffer = av_mallocz(m_videoOutBufferSize);
		av_image_fill_arrays(m_pVideoOutFrame->data,
			m_pVideoOutFrame->linesize,
			(const uint8_t*)m_pVideoOutBuffer,
			(AVPixelFormat)preset[0],
			m_pVideoCodecParam->width,
			m_pVideoCodecParam->height, 1);

		int frameSize = m_pVideoCodecParam->width * m_pVideoCodecParam->height * 3 / 2;
		setFormat(m_pVideoCodecParam->width, m_pVideoCodecParam->height, QVideoFrame::Format_YUV420P);
		m_frame.reset(new QVideoFrame(
			frameSize, 
			QSize(m_pVideoCodecParam->width, m_pVideoCodecParam->height),
			m_pVideoCodecParam->width,
			QVideoFrame::Format_YUV420P));
		connect(this, &Decoder::newVideoFrame, this, &Decoder::onNewVideoFrameReceived);
		//m_pVideowin = SDL_CreateWindow("Decoder", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, m_pVideoCodecParam->width, m_pVideoCodecParam->height, SDL_WINDOW_OPENGL);
		//m_pRender = SDL_CreateRenderer(m_pVideowin, -1, 0);
		//m_pTexture = SDL_CreateTexture(m_pRender, preset[1], SDL_TEXTUREACCESS_STREAMING, m_pVideoCodecParam->width, m_pVideoCodecParam->height);
		//m_textureRect = { 0, 0, m_pVideoCodecParam->width, m_pVideoCodecParam->height };

		m_videoDecThread = SDL_CreateThread(videoDecodeThread, "videoDecode", this);
	}
}

void Decoder::openAudioStream()
{
	AVCodec* pAudioCdec = avcodec_find_decoder(m_pAudioCodecParam->codec_id);
	if (pAudioCdec)
	{
		m_audioCodecCtx = avcodec_alloc_context3(pAudioCdec);
		avcodec_parameters_to_context(m_audioCodecCtx, m_pAudioCodecParam);
		m_audioCodecCtx->pkt_timebase = m_fmtCtx->streams[m_nAudioInx]->time_base;
		avcodec_open2(m_audioCodecCtx, pAudioCdec, nullptr);
		m_swrCtx = swr_alloc_set_opts(m_swrCtx,
			m_pAudioCodecParam->channel_layout, (AVSampleFormat)m_audioFormatPreset[0], m_pAudioCodecParam->sample_rate,
			m_pAudioCodecParam->channel_layout, (AVSampleFormat)m_pAudioCodecParam->format, m_pAudioCodecParam->sample_rate,
			-1, nullptr);
		swr_init(m_swrCtx);
		
		m_settingSpec.freq = m_pAudioCodecParam->sample_rate;
		m_settingSpec.format = m_audioFormatPreset[1];
		m_settingSpec.channels = m_pAudioCodecParam->channels;
		m_settingSpec.samples = m_pAudioCodecParam->frame_size;
		m_settingSpec.silence = 0;
		m_settingSpec.callback = audioCallback;
		m_settingSpec.userdata = this;

		//SDL_AudioSpec realSpec;
		//SDL_OpenAudio(&m_settingSpec, nullptr);

 		//SDL_PauseAudio(0);

		m_audioFormat.setCodec("audio/pcm");
		m_audioFormat.setSampleRate(m_pAudioCodecParam->sample_rate);
		m_audioFormat.setSampleSize(av_get_bytes_per_sample((AVSampleFormat)m_audioFormatPreset[0])*8);
		m_audioFormat.setSampleType(QAudioFormat::SignedInt);
		m_audioFormat.setChannelCount(m_pAudioCodecParam->channels);
		m_audioFormat.setByteOrder(QAudioFormat::LittleEndian);

		m_pAudioFrame = av_frame_alloc();
		m_audioBufferTotalSize = av_samples_get_buffer_size(
			nullptr,
			m_settingSpec.channels,
			m_settingSpec.freq,
			(AVSampleFormat)m_audioFormatPreset[0],
			1
		);
		m_audioBuff = (void*)new char[m_audioBufferTotalSize];
		m_nChannelFormatByte =
			m_settingSpec.channels * av_get_bytes_per_sample((AVSampleFormat)m_audioFormatPreset[0]);

		m_audioDecThread = SDL_CreateThread(audioDecodeThread, "audioDecode", this);
	}
}

bool Decoder::openStream(std::string filePath)
{
#ifdef SAVEPCM
	g_pcmFp = fopen("C:/Users/1/Desktop/test.pcm", "wb");
#endif

	int ret = 0;
	ret = avformat_open_input(&m_fmtCtx, filePath.c_str(), nullptr, nullptr);
	if (ret < 0)
	{
		outputError("avformat_open_input", ret);
		return false;
	}

	ret = avformat_find_stream_info(m_fmtCtx, nullptr);
	if (ret < 0)
	{
		outputError("avformat_open_input", ret);
		return false;
	}

	for (int i = 0; i < m_fmtCtx->nb_streams; ++i)
	{
		if (m_fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			m_nAudioInx = i;
			m_pAudioCodecParam = m_fmtCtx->streams[i]->codecpar;
		}
		if (m_fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			m_nVideoInx = i;
			m_pVideoCodecParam = m_fmtCtx->streams[i]->codecpar;
		}
	}

	// 不允许音视频都不存在的情况
	if (m_nAudioInx == -1 && m_nVideoInx == -1)
	{
		std::cout << "m_nAudioInx == -1 && m_nVideoInx == -1" << std::endl;
		return false;
	}

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
	// 音频相关结构体
	if (m_nAudioInx >= 0)
	{
		openAudioStream();
	}
	// 视频相关结构体
	if (m_nVideoInx >= 0)
	{
		openVideoStream();
	}

	m_pRreadPkt = av_packet_alloc();
	m_readThread = SDL_CreateThread(readThread, "readData", this);
	m_eventLoopThread = SDL_CreateThread(eventLoop, "eventLoop", this);
	
	evalCacheMax();

	emit dataReady();

	return m_nAudioInx >= 0 || m_nVideoInx >= 0; // 允许音视频分离
}

void Decoder::closeVideoStream()
{
	if (m_videoCodecCtx)
	{
		SDL_WaitThread(m_videoDecThread, NULL);
		sws_freeContext(m_swsCtx);
		av_frame_free(&m_pVideoOutFrame);
		av_frame_free(&m_pVideoFrame);
		avcodec_free_context(&m_videoCodecCtx);

		/*SDL_DestroyTexture(m_pTexture);
		SDL_DestroyRenderer(m_pRender);
		SDL_DestroyWindow(m_pVideowin);*/
	}
}

void Decoder::closeAudioStream()
{
	if (m_audioCodecCtx)
	{
		SDL_WaitThread(m_audioDecThread, NULL);
		//SDL_CloseAudio();
		delete m_audioBuff;
		swr_close(m_swrCtx);
		av_frame_free(&m_pAudioFrame);
		avcodec_free_context(&m_audioCodecCtx);
	}
}

void Decoder::closeStream()
{
	m_playControl.bAbort = true;
	closeAudioStream();
	closeVideoStream();
	SDL_WaitThread(m_readThread, NULL);
	av_packet_free(&m_pRreadPkt);
	avformat_close_input(&m_fmtCtx);
}

void Decoder::updateAudioBuffer()
{
	if (m_audioBufferSize > 0)
	{
		return;
	}

	m_audioBufferCurInx = -1;
	m_audioBufferSize = 0;
	if (m_curAudioData)
	{
		delete m_curAudioData;
		m_curAudioData = nullptr;
	}
	if (m_audioFrameQue.pop(&m_curAudioData) == QueueState::NORMAL)
	{
		m_audioBufferSize = m_curAudioData->nBufferSize;
		m_audioBufferCurInx = 0;
		m_audioClk = av_rescale_q(m_curAudioData->framePts, m_audioCodecCtx->time_base, {1, AV_TIME_BASE}); // 更新时钟
		//m_audioClk = m_audioClk * av_q2d({ 1, AV_TIME_BASE });
	}
}

bool Decoder::playAudioEof()
{
	return m_playControl.bAudioDecodeEof && m_audioFrameQue.getCurState() == QueueState::EMPTY && m_audioBufferSize == 0;
}

qint64 Decoder::readData(char* stream, qint64 len)
{
	qint64 maxLen = len;
	memset(stream, 0, len);
	if (playAudioEof())
	{
		m_playControl.bPlayAudioEof = true;
		return 0;
	}
	while (len > 0)
	{
		updateAudioBuffer();
		if (m_audioBufferSize == 0)
			break;
		int minSize = FFMIN(len, m_audioBufferSize);
		memcpy(stream, (char*)(m_curAudioData->pAudioBuffer) + m_audioBufferCurInx, minSize);
		m_audioBufferSize -= minSize;
		m_audioBufferCurInx += minSize;
		stream += minSize;
		len -= minSize;
	}
	return maxLen;
}

void Decoder::audioCallback(void* userdata, Uint8* stream, int len)
{
	memset(stream, 0, len);
	Decoder* obj = static_cast<Decoder*>(userdata);
	if (obj->playAudioEof())
	{
		obj->m_playControl.bPlayAudioEof = true;
		return;
	}
	while (len > 0)
	{
		obj->updateAudioBuffer();
		if (obj->m_audioBufferSize == 0)
			break;
		int minSize = FFMIN(len, obj->m_audioBufferSize);
		SDL_MixAudio(stream, (Uint8*)(obj->m_curAudioData->pAudioBuffer)+obj->m_audioBufferCurInx, minSize, SDL_MIX_MAXVOLUME);
		obj->m_audioBufferSize -= minSize;
		obj->m_audioBufferCurInx += minSize;
		stream += minSize;
		len -= minSize;
	}
}

// 这块还有问题
void Decoder::videoSyncClock(int lastPts)
{
	double duration = m_videoClk - lastPts; // 当前帧的持续时间
	double diff = m_videoClk - m_audioClk;
	double sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, duration));
	//std::cout << "sync_threshold=" << sync_threshold << " duration=" << duration << std::endl;
	std::cout << duration << " " << diff << std::endl;
	if (diff <= -sync_threshold)
	{
		duration = FFMAX(0, duration + diff);
	}
	else if (diff >= sync_threshold && duration > AV_SYNC_FRAMEDUP_THRESHOLD)
	{
		duration = duration + diff;
	}
	else if (diff >= sync_threshold)
	{
		duration = 2 * duration;
	}

	if (duration > 0)
	{
		av_usleep(duration);
	}
}

bool Decoder::playVideoEof()
{
	return m_playControl.bVideoDecodeEof && m_videoFrameQue.getCurState() == QueueState::EMPTY;
}

void Decoder::evalCacheMax()
{
	if (m_audioCodecCtx)
	{
		int oneSecAudioByte = m_nChannelFormatByte* m_settingSpec.freq;
		m_audioCacheMaxByte = oneSecAudioByte * PRELOADSEC;
	}
	if (m_videoCodecCtx)
	{
		int fps = m_fmtCtx->streams[m_nVideoInx]->avg_frame_rate.num;
		fps = fps <= 0? m_videoCodecCtx->framerate.num:fps;
		fps = fps <= 0? 30:fps;
		int oneVideoFrameByte = m_pVideoCodecParam->width* m_pVideoCodecParam->height * 3 / 2;
		int oneSecVideoByte = oneVideoFrameByte * fps;
		m_videoCacheMaxByte = oneSecVideoByte * PRELOADSEC;
	}	
}

void Decoder::refreshVideo()
{
	if (m_videoCodecCtx)
	{
		if (playVideoEof())
		{
			if (m_curVideoData)
			{
				delete m_curVideoData;
				m_curVideoData = nullptr;
			}
			m_playControl.bPlayVideoEof = true;
			return;
		}
		m_lastVideoData = m_curVideoData;
		if (m_videoFrameQue.pop(&m_curVideoData) == QueueState::NORMAL)
		{
			if (m_lastVideoData)
			{
				delete m_lastVideoData;
				m_lastVideoData = nullptr;
			}
			int lastClk = m_videoClk;
			m_videoClk = av_rescale_q(m_curVideoData->framePts, m_videoCodecCtx->time_base, { 1, AV_TIME_BASE });
			//m_videoClk = m_videoClk * av_q2d({ 1, AV_TIME_BASE });
			videoSyncClock(lastClk);
			if (m_frame->map(QAbstractVideoBuffer::WriteOnly)) {
				memcpy(m_frame->bits(), m_curVideoData->pVideoBuffer, size_t(m_curVideoData->nBufferSize));
				m_frame->unmap();
				emit newVideoFrame(*m_frame.get());
			};
			//SDL_RenderClear(m_pRender);
			//SDL_UpdateTexture(m_pTexture, NULL, m_curVideoData->pVideoBuffer, m_curVideoData->sdlRenderLinePixelNum);
			//SDL_RenderCopy(m_pRender, m_pTexture, NULL, &m_textureRect);
			//SDL_RenderPresent(m_pRender);
		}
	}
}

void Decoder::updatePlayControlState()
{
	m_playControl.bPlayEof = m_playControl.bPlayAudioEof && m_playControl.bPlayVideoEof;
}

int Decoder::eventLoop(void* data)
{
	Decoder* obj = static_cast<Decoder*>(data);
	while (1)
	{
		obj->updatePlayControlState();
		if (obj->m_playControl.bPlayEof)
			break;
		obj->refreshVideo();
	}
	emit obj->playFinished();
	return 0;
}

int Decoder::readThread(void* data)
{
	Decoder* obj = static_cast<Decoder*>(data);
	int ret = 0;
	while (!obj->m_playControl.bAbort)
	{
		ret = av_read_frame(obj->m_fmtCtx, obj->m_pRreadPkt);
		if (ret < 0)
		{
			obj->outputError("av_read_frame", ret);
			break;
		}
		if (obj->m_pRreadPkt->stream_index == obj->m_nAudioInx)
		{
			// 声音流
			if (obj->m_audioCodecCtx)
			{
				obj->m_audioPktQue.push(obj->m_pRreadPkt);
			}
		}
		else if (obj->m_pRreadPkt->stream_index == obj->m_nVideoInx)
		{
			// 视频流
			if (obj->m_videoCodecCtx)
			{
				obj->m_videoPktQue.push(obj->m_pRreadPkt);
			}
		}
		av_packet_unref(obj->m_pRreadPkt);
	}

	if (obj->m_audioCodecCtx)
	{
		obj->m_audioPktQue.push(nullptr);
	}
	if (obj->m_videoCodecCtx)
	{
		obj->m_videoPktQue.push(nullptr);
	}
	obj->m_playControl.bReadEof = true;
	return 0;
}

bool Decoder::isAudioCacheOverLoad()
{
	return m_audioFrameQue.totalDataByte >= m_audioCacheMaxByte;
}

int Decoder::audioDecodeThread(void* data)
{
	Decoder* obj = static_cast<Decoder*>(data);
	int recRet = 0;
	while (!obj->m_playControl.bAbort && recRet != AVERROR(EOF) && recRet != AVERROR_EOF)
	{
		// 判断是否过载
		if (obj->isAudioCacheOverLoad())
		{
			continue;
		}
		// 先取完缓存
		while (1)
		{
			recRet = avcodec_receive_frame(obj->m_audioCodecCtx, obj->m_pAudioFrame);
			if (recRet != 0)
			{
				break;
			}
			memset(obj->m_audioBuff, 0, obj->m_audioBufferTotalSize);
			int len = swr_convert(
				obj->m_swrCtx,
				(uint8_t**)&obj->m_audioBuff,
				obj->m_audioBufferTotalSize,
				(const uint8_t**)obj->m_pAudioFrame->data,
				obj->m_pAudioFrame->nb_samples);
			int audioBufferSize = len * obj->m_nChannelFormatByte;
			AudioData* audioData = new AudioData(obj->m_audioBuff,
				audioBufferSize,
				obj->m_pAudioFrame->pts);
			obj->m_audioFrameQue.push(audioData);
#ifdef SAVEPCM
			fwrite(obj->m_audioBuff, audioBufferSize, 1, g_pcmFp);
#endif
		}
		// 需要输入
		if (recRet == AVERROR(EAGAIN))
		{
			AVPacket* pkt = av_packet_alloc();
			QueueState ret = obj->m_audioPktQue.pop(pkt);
			if (ret == QueueState::NORMAL)
			{
				av_packet_rescale_ts(pkt, obj->m_fmtCtx->streams[obj->m_nAudioInx]->time_base, obj->m_audioCodecCtx->time_base);
				avcodec_send_packet(obj->m_audioCodecCtx, pkt);
			}
			else if (ret == QueueState::LAST)
			{
				avcodec_send_packet(obj->m_audioCodecCtx, nullptr);
			}
			av_packet_free(&pkt);
		}
	}
	obj->m_playControl.bAudioDecodeEof = true;
#ifdef SAVEPCM
	fclose(g_pcmFp);
#endif
	return 0;
}

bool Decoder::isVideoCacheOverLoad()
{
	return m_videoFrameQue.totalDataByte >= m_videoCacheMaxByte;
}

int Decoder::videoDecodeThread(void* data)
{
	Decoder* obj = static_cast<Decoder*>(data);
	int recRet = 0;
	while (!obj->m_playControl.bAbort && recRet != AVERROR(EOF) && recRet != AVERROR_EOF)
	{
		// 判断是否过载
		if (obj->isVideoCacheOverLoad())
		{
			continue;
		}
		// 先取完解码器的数据
		while (1)
		{
			recRet = avcodec_receive_frame(obj->m_videoCodecCtx, obj->m_pVideoFrame);
			if (recRet != 0)
			{
				break;
			}
			sws_scale(
				obj->m_swsCtx,
				(const uint8_t* const*)obj->m_pVideoFrame->data, obj->m_pVideoFrame->linesize,
				0, obj->m_pVideoCodecParam->height,
				obj->m_pVideoOutFrame->data, obj->m_pVideoOutFrame->linesize);
			VideoData *videoData = new VideoData(obj->m_pVideoOutBuffer, 
				obj->m_videoOutBufferSize, 
				obj->m_pVideoOutFrame->linesize[0],
				obj->m_pVideoFrame->pts);
			obj->m_videoFrameQue.push(videoData);
			av_frame_unref(obj->m_pVideoFrame);
		}
		// 需要输入
		if (recRet == AVERROR(EAGAIN))
		{
			AVPacket *pkt = av_packet_alloc();
			QueueState ret = obj->m_videoPktQue.pop(pkt);
			if (ret == QueueState::NORMAL)
			{
				auto it1 = pkt->pts;
				av_packet_rescale_ts(pkt, obj->m_fmtCtx->streams[obj->m_nVideoInx]->time_base, obj->m_videoCodecCtx->time_base);
				auto it2 = pkt->pts;
				avcodec_send_packet(obj->m_videoCodecCtx, pkt);
			}
			else if (ret == QueueState::LAST)
			{
				avcodec_send_packet(obj->m_videoCodecCtx, nullptr);
			}
			av_packet_free(&pkt);
		}
	}
	obj->m_playControl.bVideoDecodeEof = true;
	return 0;
}

void Decoder::outputError(std::string&& funName, int ret)
{
	char buffer[1024] = {0};
	av_strerror(ret, buffer, sizeof(buffer));
	std::cout << "[function]:" << funName << " [reason]:" << buffer << std::endl;
}
