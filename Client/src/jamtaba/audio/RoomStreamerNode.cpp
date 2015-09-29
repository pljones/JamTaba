#include "RoomStreamerNode.h"
#include "codec.h"
#include "core/AudioDriver.h"
#include "core/SamplesBuffer.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QUrl>
#include <QNetworkReply>
#include <QDebug>
#include <QBuffer>
#include <QObject>
#include <QDateTime>
#include <QWaitCondition>
#include <cmath>
#include <QMutexLocker>
#include <QFile>

Q_LOGGING_CATEGORY(roomStreamer, "roomStreamer")

using namespace Audio;

const int AbstractMp3Streamer::MAX_BYTES_PER_DECODING = 2048;

//+++++++++++++
AbstractMp3Streamer::AbstractMp3Streamer( Audio::Mp3Decoder *decoder)
    :
      faderProcessor(0, 1, 44100*3),//3 seconds fade in
      decoder(decoder),
      device(nullptr),
      streaming(false),
      bufferedSamples(2, 4096)
{
    //addProcessor(faderProcessor);
    bufferedSamples.setFrameLenght(0);//reset internal offset
}

AbstractMp3Streamer::~AbstractMp3Streamer(){
	delete decoder;
}

void AbstractMp3Streamer::stopCurrentStream(){
    qCDebug(roomStreamer) << "stopping room stream";

    //QMutexLocker locker(&mutex);
    faderProcessor.reset();//aply fadein in next stream
    if(device){
        //device->close();
        decoder->reset();//discard unprocessed bytes
        device->deleteLater();
        device = nullptr;
        bufferedSamples.zero();//discard samples
        streaming = false;
    }
    bytesToDecode.clear();
}

int AbstractMp3Streamer::getSamplesToRender(int targetSampleRate, int outLenght){
    bool needResampling =  needResamplingFor(targetSampleRate);
    int samplesToRender = needResampling ? getInputResamplingLength(getSampleRate(), targetSampleRate, outLenght) : outLenght;
    return samplesToRender;
}

void AbstractMp3Streamer::processReplacing(const Audio::SamplesBuffer &in, Audio::SamplesBuffer &out, int targetSampleRate, const Midi::MidiBuffer &midiBuffer){
    Q_UNUSED(in);
    //QMutexLocker locker(&mutex);

    if(bufferedSamples.isEmpty() || !streaming){
        return;
    }

    int outLenght = std::min(out.getFrameLenght(), bufferedSamples.getFrameLenght());
    int samplesToRender = getSamplesToRender(targetSampleRate, outLenght);
    if(samplesToRender <= 0){
        return;
    }

    internalInputBuffer.setFrameLenght(samplesToRender);
    internalInputBuffer.set(bufferedSamples);

    if( needResamplingFor(targetSampleRate)){
        const Audio::SamplesBuffer& resampledBuffer = resampler.resample(internalInputBuffer, outLenght );
        internalOutputBuffer.setFrameLenght(resampledBuffer.getFrameLenght());
        internalOutputBuffer.set(resampledBuffer);
    }
    else{
        internalOutputBuffer.setFrameLenght(out.getFrameLenght());
        internalOutputBuffer.set(internalInputBuffer);
    }

    //qWarning() << "samples to render:"<< samplesToRender << " out.frameLenght:" << out.getFrameLenght() << " bufferedSamples:" << bufferedSamples.getFrameLenght();

    //internalOutputBuffer.setFrameLenght(samplesToRender);
    //internalOutputBuffer.zero();
    //internalOutputBuffer.set(bufferedSamples);//copy 'samplesToRender' samples to internalOutputBuffer

    //qCDebug(roomStreamer) << "discarding " << samplesToRender << " samples";
    bufferedSamples.discardFirstSamples(samplesToRender);//keep non rendered samples for next audio callback
    //qCDebug(roomStreamer) << "new frameLenght:" << bufferedSamples.getFrameLenght();

    if(internalOutputBuffer.getFrameLenght() < out.getFrameLenght()){
        qCDebug(roomStreamer) << out.getFrameLenght() - internalOutputBuffer.getFrameLenght() << " samples missing";
    }

    //faderProcessor.process(internalInputBuffer, internalOutputBuffer, midiBuffer);//aply fade in in stream

    this->lastPeak.update(internalOutputBuffer.computePeak());

    out.add(internalOutputBuffer);
}

void AbstractMp3Streamer::initialize(QString streamPath){
    //Q_UNUSED(streamPath);
    streaming = !streamPath.isNull() && !streamPath.isEmpty();
}

int AbstractMp3Streamer::getSampleRate() const{
    return decoder->getSampleRate();
}

bool AbstractMp3Streamer::needResamplingFor(int targetSampleRate) const{
    if(!streaming ){
        return false;
    }
    return targetSampleRate != getSampleRate();
}

void AbstractMp3Streamer::decode(const unsigned int maxBytesToDecode){
    //QMutexLocker locker(&mutex);
    if(!device ){
        return;
    }

    qint64 totalBytesToProcess = std::min((int)maxBytesToDecode, bytesToDecode.size());
    char* in = bytesToDecode.data();

    if(totalBytesToProcess > 0){
        int bytesProcessed = 0;
        while(bytesProcessed < totalBytesToProcess){//split bytesReaded in chunks to avoid a very large decoded buffer
            int bytesToProcess = std::min((int)(totalBytesToProcess - bytesProcessed), MAX_BYTES_PER_DECODING);//chunks maxsize is 2048 bytes
            const Audio::SamplesBuffer* decodedBuffer = decoder->decode(in, bytesToProcess);
            //prepare in for the next decoding
            in += bytesToProcess;
            bytesProcessed += bytesToProcess;
            //+++++++++++++++++  PROCESS DECODED SAMPLES ++++++++++++++++
            bufferedSamples.append(*decodedBuffer);
        }
        bytesToDecode = bytesToDecode.right(bytesToDecode.size() - bytesProcessed);
        if(bytesToDecode.isEmpty()){
            qCDebug(roomStreamer) << bytesProcessed << " decoded  bytesToDecode: " << bytesToDecode.size();
        }
    }
}

void AbstractMp3Streamer::setStreamPath(QString streamPath){
    stopCurrentStream();
    initialize(streamPath);
}

//+++++++++++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++
NinjamRoomStreamerNode::NinjamRoomStreamerNode(QUrl streamPath, int bufferTimeInSeconds)
    : AbstractMp3Streamer( new Mp3DecoderMiniMp3()),
      bufferTime(bufferTimeInSeconds),
      httpClient(nullptr)
      //osc(440, 44100)
{

    setStreamPath(streamPath.toString());
}

NinjamRoomStreamerNode::NinjamRoomStreamerNode(int bufferTimeInSeconds)
    :AbstractMp3Streamer( new Mp3DecoderMiniMp3()),
      bufferTime(bufferTimeInSeconds),
      httpClient(nullptr)
      //osc(440, 44100)
{
    setStreamPath("");
}


bool NinjamRoomStreamerNode::needResamplingFor(int targetSampleRate) const{
    if(!streaming || buffering){
        return false;
    }
    return  AbstractMp3Streamer::needResamplingFor(targetSampleRate);
}

void NinjamRoomStreamerNode::initialize(QString streamPath){
    AbstractMp3Streamer::initialize(streamPath);
    buffering =  true;
    bufferedSamples.zero();
    bytesToDecode.clear();
    if(!streamPath.isEmpty()){
        qCDebug(roomStreamer) << "connecting in " << streamPath;
        if(httpClient){
            httpClient->deleteLater();
        }
        httpClient = new QNetworkAccessManager(this);
        QNetworkReply* reply = httpClient->get(QNetworkRequest(QUrl(streamPath)));
        QObject::connect(reply, SIGNAL(readyRead()), this, SLOT(on_reply_read()));
        QObject::connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(on_reply_error(QNetworkReply::NetworkError)));
        this->device = reply;
    }
}

void NinjamRoomStreamerNode::on_reply_error(QNetworkReply::NetworkError /*error*/){
    QString msg = "ERROR playing room stream";
    qCCritical(roomStreamer) << msg;
    emit error(msg);
}

void NinjamRoomStreamerNode::on_reply_read(){
    if(!device){
        qCDebug(roomStreamer) << "device is null!";
        return;
    }
    if(device->isOpen() && device->isReadable()){
        QMutexLocker locker(&mutex);
        bytesToDecode.append(device->readAll());
        qCDebug(roomStreamer) << "bytes downloaded  bytesToDecode:"<<bytesToDecode.size() << " bufferedSamples: " << bufferedSamples.getFrameLenght();
    }
    else{
        qCCritical(roomStreamer) << "problem in device!";
    }
}

NinjamRoomStreamerNode::~NinjamRoomStreamerNode(){
    qCDebug(roomStreamer) << "RoomStreamerNode destructor!";
}

void NinjamRoomStreamerNode::processReplacing(const SamplesBuffer & in, SamplesBuffer &out, int sampleRate, const Midi::MidiBuffer &midiBuffer){

    QMutexLocker locker(&mutex);
    if(!buffering){
        int outLenght = out.getFrameLenght();// std::min(out.getFrameLenght(), bufferedSamples.getFrameLenght());
        int samplesToRender = getSamplesToRender( sampleRate, outLenght);
        if(bufferedSamples.getFrameLenght() < samplesToRender){
            decode(4096);

        }
        //decode(256);
    }
    //else{
        decode(4096);
    //}

    //AbstractMp3Streamer::processReplacing(in, out, sampleRate, midiBuffer);

    qWarning() << "Buffered samples:" << bufferedSamples.getFrameLenght() << " bytesToDecode:" << bytesToDecode.size();
    //return;

    if(buffering && bufferedSamples.getFrameLenght() >= bufferTime * decoder->getSampleRate()){
        buffering = false;
    }

    if(!buffering){
        AbstractMp3Streamer::processReplacing(in, out, sampleRate, midiBuffer);
    }
    else{//buffering
        lastPeak.zero();
    }
}

//++++++++++++++++++

AudioFileStreamerNode::AudioFileStreamerNode(QString file)
    :   AbstractMp3Streamer( new Mp3DecoderMiniMp3())
{
    setStreamPath(file);
}

void AudioFileStreamerNode::initialize(QString streamPath){
    AbstractMp3Streamer::initialize(streamPath);
    QFile* f = new QFile(streamPath);
    if(!f->open(QIODevice::ReadOnly)){
        qCCritical(roomStreamer) << "não foi possivel abrir o arquivo " << streamPath;
    }
    this->device = f;
    bytesToDecode.append(f->readAll());
}

AudioFileStreamerNode::~AudioFileStreamerNode(){
    //device->deleteLater();
}

void AudioFileStreamerNode::processReplacing(const SamplesBuffer & in, SamplesBuffer &out, int sampleRate, const Midi::MidiBuffer &midiBuffer){
    while(bufferedSamples.getFrameLenght() < out.getFrameLenght()){
        //bytesToDecode.append(device->read(1024 + 256));
        decode(1024 + 1024);
    }


    AbstractMp3Streamer::processReplacing(in, out, sampleRate, midiBuffer);
}
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++/*

TestStreamerNode::TestStreamerNode(int sampleRate)
    :AbstractMp3Streamer(new Mp3DecoderMiniMp3())
{
    oscilator = new OscillatorAudioNode(440, sampleRate);
    playing = true;
}

TestStreamerNode::~TestStreamerNode(){
    delete oscilator;
}

void TestStreamerNode::initialize(QString streamPath){
    AbstractMp3Streamer::initialize(streamPath);
}

void TestStreamerNode::processReplacing(const Audio::SamplesBuffer & in, Audio::SamplesBuffer &out, int sampleRate, const Midi::MidiBuffer &midiBuffer){

    if(playing){
        oscilator->processReplacing(in, out, sampleRate, midiBuffer);
        //faderProcessor.process(in, out, midiBuffer);
    }
    lastPeak.update( out.computePeak());
}


void TestStreamerNode::setStreamPath(QString streamPath){
    faderProcessor.reset();
    playing = true;
    bytesToDecode.clear();
}

void TestStreamerNode::stopCurrentStream(){
    playing = false;
}

