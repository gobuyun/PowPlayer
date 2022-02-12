#include "AudioOutput.h"

AudioOutput::AudioOutput(QObject* parents) : QObject(parents)
{
}

void AudioOutput::setSource(Decoder* source)
{
	if (!source) return;
	connect(source, &Decoder::dataReady, this, &AudioOutput::onDataReady);
	m_pSourceObj = source;
}

void AudioOutput::onDataReady()
{
	m_audioFormat = m_pSourceObj->getAudioFormat();
	m_audioOutput = new QAudioOutput(*m_audioFormat, this);
	m_pSourceObj->open(QIODevice::ReadOnly);
	m_audioOutput->start(m_pSourceObj);
}
