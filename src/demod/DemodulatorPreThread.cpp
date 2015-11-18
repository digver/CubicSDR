#include "CubicSDRDefs.h"
#include <vector>

#ifdef __APPLE__
#include <pthread.h>
#endif

#include "DemodulatorPreThread.h"
#include "CubicSDR.h"

DemodulatorPreThread::DemodulatorPreThread() : IOThread(), iqResampler(NULL), iqResampleRatio(1), iqInputQueue(NULL), iqOutputQueue(NULL), threadQueueNotify(NULL), commandQueue(NULL), cModem(nullptr), cModemKit(nullptr)
 {
	initialized.store(false);

    freqShifter = nco_crcf_create(LIQUID_VCO);
    shiftFrequency = 0;

    workerQueue = new DemodulatorThreadWorkerCommandQueue;
    workerResults = new DemodulatorThreadWorkerResultQueue;
     
    workerThread = new DemodulatorWorkerThread();
    workerThread->setInputQueue("WorkerCommandQueue",workerQueue);
    workerThread->setOutputQueue("WorkerResultQueue",workerResults);
}

void DemodulatorPreThread::initialize() {
    iqResampleRatio = (double) (params.bandwidth) / (double) params.sampleRate;

    float As = 60.0f;         // stop-band attenuation [dB]

    iqResampler = msresamp_crcf_create(iqResampleRatio, As);

    initialized.store(true);
    
    lastParams = params;
}

DemodulatorPreThread::~DemodulatorPreThread() {
}

void DemodulatorPreThread::run() {
#ifdef __APPLE__
    pthread_t tID = pthread_self();  // ID of this thread
    int priority = sched_get_priority_max( SCHED_FIFO) - 1;
    sched_param prio = {priority}; // scheduling priority of thread
    pthread_setschedparam(tID, SCHED_FIFO, &prio);
#endif

    if (!initialized) {
        initialize();
    }

    std::cout << "Demodulator preprocessor thread started.." << std::endl;

    t_Worker = new std::thread(&DemodulatorWorkerThread::threadMain, workerThread);

    ReBuffer<DemodulatorThreadPostIQData> buffers;

    iqInputQueue = (DemodulatorThreadInputQueue*)getInputQueue("IQDataInput");
    iqOutputQueue = (DemodulatorThreadPostInputQueue*)getOutputQueue("IQDataOutput");
    threadQueueNotify = (DemodulatorThreadCommandQueue*)getOutputQueue("NotifyQueue");
    commandQueue = ( DemodulatorThreadCommandQueue*)getInputQueue("CommandQueue");
    
    std::vector<liquid_float_complex> in_buf_data;
    std::vector<liquid_float_complex> out_buf_data;

    setDemodType(params.demodType);
    
    while (!terminated) {
        DemodulatorThreadIQData *inp;
        iqInputQueue->pop(inp);

        bool bandwidthChanged = false;
        bool rateChanged = false;
        DemodulatorThreadParameters tempParams = params;

        if (!commandQueue->empty()) {
            while (!commandQueue->empty()) {
                DemodulatorThreadCommand command;
                commandQueue->pop(command);
                switch (command.cmd) {
                case DemodulatorThreadCommand::DEMOD_THREAD_CMD_SET_BANDWIDTH:
                    if (command.llong_value < 1500) {
                        command.llong_value = 1500;
                    }
                    if (command.llong_value > params.sampleRate) {
                        tempParams.bandwidth = params.sampleRate;
                    } else {
                        tempParams.bandwidth = command.llong_value;
                    }
                    bandwidthChanged = true;
                    break;
                case DemodulatorThreadCommand::DEMOD_THREAD_CMD_SET_FREQUENCY:
                    params.frequency = tempParams.frequency = command.llong_value;
                    break;
                case DemodulatorThreadCommand::DEMOD_THREAD_CMD_SET_AUDIO_RATE:
                    tempParams.audioSampleRate = (int)command.llong_value;
                    rateChanged = true;
                    break;
                default:
                    break;
                }
            }
        }

        if (inp->sampleRate != tempParams.sampleRate && inp->sampleRate) {
            tempParams.sampleRate = inp->sampleRate;
            rateChanged = true;
        }

        if (bandwidthChanged || rateChanged) {
            DemodulatorWorkerThreadCommand command(DemodulatorWorkerThreadCommand::DEMOD_WORKER_THREAD_CMD_BUILD_FILTERS);
            command.sampleRate = tempParams.sampleRate;
            command.audioSampleRate = tempParams.audioSampleRate;
            command.bandwidth = tempParams.bandwidth;
            command.frequency = tempParams.frequency;

            workerQueue->push(command);
        }

        if (!initialized) {
            inp->decRefCount();
            continue;
        }

        // Requested frequency is not center, shift it into the center!
        if ((params.frequency - inp->frequency) != shiftFrequency || rateChanged) {
            shiftFrequency = params.frequency - inp->frequency;
            if (abs(shiftFrequency) <= (int) ((double) (inp->sampleRate / 2) * 1.5)) {
                nco_crcf_set_frequency(freqShifter, (2.0 * M_PI) * (((double) abs(shiftFrequency)) / ((double) inp->sampleRate)));
            }
        }

        if (abs(shiftFrequency) > (int) ((double) (inp->sampleRate / 2) * 1.5)) {
            inp->decRefCount();
            continue;
        }

//        std::lock_guard < std::mutex > lock(inp->m_mutex);
        std::vector<liquid_float_complex> *data = &inp->data;
        if (data->size() && (inp->sampleRate == params.sampleRate)) {
            int bufSize = data->size();

            if (in_buf_data.size() != bufSize) {
                if (in_buf_data.capacity() < bufSize) {
                    in_buf_data.reserve(bufSize);
                    out_buf_data.reserve(bufSize);
                }
                in_buf_data.resize(bufSize);
                out_buf_data.resize(bufSize);
            }

            in_buf_data.assign(inp->data.begin(), inp->data.end());

            liquid_float_complex *in_buf = &in_buf_data[0];
            liquid_float_complex *out_buf = &out_buf_data[0];
            liquid_float_complex *temp_buf = NULL;

            if (shiftFrequency != 0) {
                if (shiftFrequency < 0) {
                    nco_crcf_mix_block_up(freqShifter, in_buf, out_buf, bufSize);
                } else {
                    nco_crcf_mix_block_down(freqShifter, in_buf, out_buf, bufSize);
                }
                temp_buf = in_buf;
                in_buf = out_buf;
                out_buf = temp_buf;
            }

            DemodulatorThreadPostIQData *resamp = buffers.getBuffer();

            int out_size = ceil((double) (bufSize) * iqResampleRatio) + 512;

            if (resampledData.size() != out_size) {
                if (resampledData.capacity() < out_size) {
                    resampledData.reserve(out_size);
                }
                resampledData.resize(out_size);
            }

            unsigned int numWritten;
            msresamp_crcf_execute(iqResampler, in_buf, bufSize, &resampledData[0], &numWritten);

            resamp->setRefCount(1);
            resamp->data.assign(resampledData.begin(), resampledData.begin() + numWritten);

            resamp->modemType = demodType;
            resamp->modem = cModem;
            resamp->modemKit = cModemKit;
            resamp->sampleRate = params.bandwidth;

            iqOutputQueue->push(resamp);
        }

        inp->decRefCount();

        if (!terminated && !workerResults->empty()) {
            while (!workerResults->empty()) {
                DemodulatorWorkerThreadResult result;
                workerResults->pop(result);

                switch (result.cmd) {
                case DemodulatorWorkerThreadResult::DEMOD_WORKER_THREAD_RESULT_FILTERS:

                    if (result.iqResampler) {
                        if (iqResampler) {
                            msresamp_crcf_destroy(iqResampler);
                        }
                        iqResampler = result.iqResampler;
                        iqResampleRatio = result.iqResampleRatio;
                    }

                    if (result.modem != nullptr) {
                        cModem = result.modem;
                    }
                    
                    if (result.modemKit != nullptr) {
                        cModemKit = result.modemKit;
                    }
                        
                    if (result.bandwidth) {
                        params.bandwidth = result.bandwidth;
                    }

                    if (result.sampleRate) {
                        params.sampleRate = result.sampleRate;
                    }
                        
                    if (result.modemType != "") {
                        demodType = result.modemType;
                        params.demodType = result.modemType;
                        demodTypeChanged.store(false);
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }

    buffers.purge();

    DemodulatorThreadCommand tCmd(DemodulatorThreadCommand::DEMOD_THREAD_CMD_DEMOD_PREPROCESS_TERMINATED);
    tCmd.context = this;
    threadQueueNotify->push(tCmd);
    std::cout << "Demodulator preprocessor thread done." << std::endl;
}

DemodulatorThreadParameters &DemodulatorPreThread::getParams() {
    return params;
}

void DemodulatorPreThread::setParams(DemodulatorThreadParameters &params_in) {
    params = params_in;
}

void DemodulatorPreThread::setDemodType(std::string demodType) {
    this->newDemodType = demodType;
    DemodulatorWorkerThreadCommand command(DemodulatorWorkerThreadCommand::DEMOD_WORKER_THREAD_CMD_MAKE_DEMOD);
    command.demodType = demodType;
    command.bandwidth = params.bandwidth;
    command.audioSampleRate = params.audioSampleRate;
    workerQueue->push(command);
}

std::string DemodulatorPreThread::getDemodType() {
    if (newDemodType != demodType) {
        return newDemodType;
    }
    return demodType;
}

void DemodulatorPreThread::terminate() {
    terminated = true;
    DemodulatorThreadIQData *inp = new DemodulatorThreadIQData;    // push dummy to nudge queue
    iqInputQueue->push(inp);
    DemodulatorWorkerThreadCommand command(DemodulatorWorkerThreadCommand::DEMOD_WORKER_THREAD_CMD_NULL);
    workerQueue->push(command);
    workerThread->terminate();
    t_Worker->join();
    delete t_Worker;
    delete workerThread;
    delete workerResults;
    delete workerQueue;
}
