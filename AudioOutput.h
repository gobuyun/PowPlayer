#pragma once

#include <QObject>
#include <QAudioFormat>
#include <QAudioOutput>

#include "Decoder.h"

class AudioOutput : public QObject
{
	Q_OBJECT
	Q_PROPERTY(Decoder* source READ getSource WRITE setSource);

public:
	AudioOutput(QObject* parents=nullptr);

	Decoder* getSource()
	{
		return m_pSourceObj;
	}
	void setSource(Decoder* source);

public slots:
	void onDataReady();

private:
	QAudioFormat* m_audioFormat;
	QAudioOutput* m_audioOutput = nullptr;
	Decoder* m_pSourceObj = nullptr;
};
