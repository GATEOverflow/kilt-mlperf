//
// MIT License
//
// Copyright (c) 2021 - 2023 Krai Ltd
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.POSSIBILITY OF SUCH DAMAGE.
//

#ifndef DEVICE_H
#define DEVICE_H

#include <queue>

#include "api/master/QAicInfApi.h"
#include "config/device_config.h"
#include "idatasource.h"
#include "imodel.h"

//#define NO_QAIC
//#define ENQUEUE_SHIM_THREADED
//#define ENQUEUE_SHIM_THREADED_COUNT 2
#define ENQUEUE_SHIM_THREADED_COUNT 0

using namespace KRAI;
using namespace qaic_api;

template <typename Sample> class Device;

template <typename Sample> struct Payload {
  std::vector<Sample> samples;
  int device;
  int activation;
  int set;
  Device<Sample> *dptr;
};

template <typename Sample> class RingBuffer {

public:
  RingBuffer(int d, int a, int s) {
    size = s;
    for (int i = 0; i < s; ++i) {
      auto p = new Payload<Sample>;
      p->set = i;
      p->activation = a;
      p->device = d;
      q.push(p);
    }
  }

  RingBuffer(int d, int a, int s, Device<Sample> *dptr) {
    size = s;
    for (int i = 0; i < s; ++i) {
      auto p = new Payload<Sample>;
      p->set = i;
      p->activation = a;
      p->device = d;
      p->dptr = dptr;
      q.push(p);
    }
  }

  virtual ~RingBuffer() {
    while (!q.empty()) {
      auto f = q.front();
      q.pop();
      delete f;
    }
  }

  Payload<Sample> *getPayload() {
    std::unique_lock<std::mutex> lock(mtx);
    if (q.empty())
      return nullptr;
    else {
      auto f = q.front();
      q.pop();
      return f;
    }
  }

  void release(Payload<Sample> *p) {
    std::unique_lock<std::mutex> lock(mtx);
    // std::cout << "Release before: " << front << " end: " << end << std::endl;
    if (q.size() == size || p == nullptr) {
      std::cerr << "extra elem in the queue" << std::endl;
      // assert(1);
    }
    q.push(p);
    // std::cout << "Release after: " << front << " end: " << end << std::endl;
  }

  void debug() {
    // std::cout << "QUEUE front: " << front << " end: " << end << std::endl;
  }

private:
  std::queue<Payload<Sample> *> q;
  int size;
  bool isEmpty;
  std::mutex mtx;
};

typedef void (*DeviceExec)(void *data);

template <typename Sample> class Device : public IDevice<Sample> {

public:
  void Construct(IModel *_model, IDataSource *_data_source, IConfig *_config,
                 int hw_id, std::vector<int> aff) {

    QAicDeviceConfig *device_cfg =
        static_cast<QAicDeviceConfig *>(_config->device_cfg);

    cpu_set_t cpu_affinity;

    std::vector<int> aff_cpy = aff;
    int device_threads = 0;

    if (device_cfg->ringfenceDeviceDriver()) {
      device_threads = 1;
    }

    std::cout << "Driver threads: ";
    CPU_ZERO(&cpu_affinity);
    for (int a = aff.size(); a > device_threads; a--) {
      CPU_SET(aff.back(), &cpu_affinity);
      std::cout << " " << aff.back();
      aff.pop_back();
    }
    std::cout << std::endl;

    if (!device_cfg->ringfenceDeviceDriver()) {
      aff = aff_cpy;
    }

    mtx_init.lock();
    std::thread tin(&Device::DeviceInitMutex, this, _model, _data_source,
                    _config, hw_id, &aff);
    pthread_setaffinity_np(tin.native_handle(), sizeof(cpu_set_t),
                           &cpu_affinity);
    mtx_init.unlock();

    tin.join();
  }

  virtual int Inference(std::vector<Sample> samples) {

    if (sback >= sfront + samples_queue_depth)
      return -1;

    samples_queue[sback % samples_queue_depth] = samples;
    ++sback;

    return samples_queue_depth - (sback - sfront);
  }

  ~Device() {
    scheduler_terminate = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    scheduler.join();

#ifdef ENQUEUE_SHIM_THREADED
    shim_terminate = true;
    for (int i = 0; i < num_setup_threads; ++i)
      payload_threads[i].join();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif

#ifndef NO_QAIC
    delete runner;
#endif
  }

private:
  virtual void DeviceInit(IModel *_model, IDataSource *_data_source,
                          IConfig *_config, int hw_id, std::vector<int> *aff) {

    scheduler_terminate = false;

    model = _model;
    data_source = _data_source;

    device_cfg = static_cast<QAicDeviceConfig *>(_config->device_cfg);

    model_cfg = static_cast<IModelConfig *>(_config->model_cfg);

    samples_queue_depth = device_cfg->getSamplesQueueDepth();

    scheduler_yield_time = device_cfg->getSchedulerYieldTime();
    enqueue_yield_time = device_cfg->getEnqueueYieldTime();

    loop_back = device_cfg->getLoopback();

#ifndef NO_QAIC

    std::cout << "Creating device " << hw_id << std::endl;
    runner = new QAicInfApi();

    runner->setModelBasePath(device_cfg->getModelRoot());
    runner->setNumActivations(device_cfg->getActivationCount());
    runner->setSetSize(device_cfg->getSetSize());
    runner->setNumThreadsPerQueue(device_cfg->getNumThreadsPerQueue());
    runner->setSkipStage(device_cfg->getSkipStage());

    QStatus status = runner->init(hw_id, PostResults);

    if (status != QS_SUCCESS)
      throw "Failed to invoke qaic";

    buffers_in.resize(device_cfg->getActivationCount());
    buffers_out.resize(device_cfg->getActivationCount());

    std::cout << "Model input count: " << model_cfg->getInputCount()
              << std::endl;
    std::cout << "Model output count: " << model_cfg->getOutputCount()
              << std::endl;

    // get references to all the buffers
    for (int a = 0; a < device_cfg->getActivationCount(); ++a) {
      buffers_in[a].resize(device_cfg->getSetSize());
      buffers_out[a].resize(device_cfg->getSetSize());
      for (int s = 0; s < device_cfg->getSetSize(); ++s) {
        for (int i = 0; i < model_cfg->getInputCount(); ++i) {
          buffers_in[a][s].push_back((void *)runner->getBufferPtr(a, s, i));
        }
        for (int o = 0; o < model_cfg->getOutputCount(); ++o) {
          buffers_out[a][s].push_back((void *)runner->getBufferPtr(
              a, s, o + model_cfg->getInputCount()));
        }
      }
    }

#else
    std::cout << "Creating dummy device " << hw_id << std::endl;
#endif

    // create enough ring buffers for each activation
    ring_buf.resize(device_cfg->getActivationCount());

    // populate ring buffer
    for (int a = 0; a < device_cfg->getActivationCount(); ++a)
      ring_buf[a] =
          new RingBuffer<Sample>(0, a, device_cfg->getSetSize(), this);

    samples_queue.resize(samples_queue_depth);
    sfront = sback = 0;

    // Kick off the scheduler
    scheduler = std::thread(&Device::QueueScheduler, this);

    // Set the affinity of the scheduler
    std::cout << "Scheduler thread " << aff->back() << std::endl;

    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);

    int cpu = aff->back();
    CPU_SET(cpu, &cpu_set);

    pthread_setaffinity_np(scheduler.native_handle(), sizeof(cpu_set_t),
                           &cpu_set);

    aff->pop_back();

#ifdef ENQUEUE_SHIM_THREADED
    shim_terminate = false;

    num_setup_threads = ENQUEUE_SHIM_THREADED_COUNT;

    payloads.resize(num_setup_threads, nullptr);

    for (int i = 0; i < num_setup_threads; ++i) {

      std::cout << "Shim thread " << aff->back() << std::endl;
      payload_threads.push_back(std::thread(&Device::EnqueueShim, this, i));

      // set the affinity of the shim
      cpu_set_t cpu_set;
      CPU_ZERO(&cpu_set);

      int cpu = aff->back();

      CPU_SET(cpu, &cpu_set);

      pthread_setaffinity_np(payload_threads.back().native_handle(),
                             sizeof(cpu_set_t), &cpu_set);

      aff->pop_back();
    }

#else
    payloads.resize(1, nullptr);
    shim_terminate = true;
#endif
  }

  void EnqueueShim(int id) {

    do {
      // std::cout << "Shim " << sched_getcpu() << std::endl;
      if (payloads[id] != nullptr) {
        Payload<Sample> *p = payloads[id];

#ifndef NO_QAIC
        // set the images
        if (device_cfg->getInputSelect() == 0) {
          model->configureWorkload(data_source, &(p->samples),
                                   buffers_in[p->activation][p->set]);
        } else if (device_cfg->getInputSelect() == 1) {
          assert(false);
          // TODO: We no longer support this as we're copying directly into the
          // buffer?
          // void *samples_ptr = session->getSamplePtr(p->samples[0].index);
          // runner->setBufferPtr(p->activation, p->set, 0, samples_ptr);
        } else {
          // Do nothing - random data
        }

        if (loop_back) {
          PostResults(NULL, QAIC_EVENT_DEVICE_COMPLETE, p);
        } else {
          // std::cout << "Issuing to hardware" << std::endl;
          QStatus status = runner->run(p->activation, p->set, p);
          if (status != QS_SUCCESS)
            throw "Failed to invoke qaic";
        }
#else
        PostResults(NULL, QAIC_EVENT_DEVICE_COMPLETE, p);
#endif

        payloads[id] = nullptr;
      } else {
        if (enqueue_yield_time)
          std::this_thread::sleep_for(
              std::chrono::microseconds(enqueue_yield_time));
      }

    } while (!shim_terminate);
  }

  void QueueScheduler() {

    // current activation index
    int activation = -1;

    std::vector<Sample> qs(model_cfg->getBatchSize());

    while (!scheduler_terminate) { // loop forever waiting for input
      // std::cout << "Scheduler " << sched_getcpu() << std::endl;
      if (sfront == sback) {
        // No samples then post last results and continue
        // Device::PostResults(nullptr, QAIC_EVENT_DEVICE_COMPLETE, this);
        if (scheduler_yield_time)
          std::this_thread::sleep_for(
              std::chrono::microseconds(scheduler_yield_time));

        continue;
      }

      // copy the image list and remove from queue
      qs = samples_queue[sfront % samples_queue_depth];
      ++sfront;

      // if(config->getVerbosityServer())
      //  std::cout << "<" << sback - sfront << ">";

      while (!scheduler_terminate) {

        activation = (activation + 1) % device_cfg->getActivationCount();

        Payload<Sample> *p = ring_buf[activation]->getPayload();

        // if no hardware slots available then increment the activation
        // count and then continue
        if (p == nullptr) {
          if (scheduler_yield_time)
            std::this_thread::sleep_for(
                std::chrono::microseconds(scheduler_yield_time));
          continue;
        }

        // add the image samples to the payload
        p->samples = qs;

#ifdef ENQUEUE_SHIM_THREADED
        int round_robin = 0;

        while (payloads[round_robin] != nullptr) {
          std::this_thread::sleep_for(std::chrono::microseconds(1));
        }

        payloads[round_robin] = p;

        // std::cout << " " << round_robin;
        round_robin = (round_robin + 1) % num_setup_threads;
#else
        // place the payload in the first slot and call shim directly
        payloads[0] = p;
        EnqueueShim(0);
#endif
        break;
      }
    }
    std::cout << "QAIC Device Scheduler terminating..." << std::endl;
  }

  static void PostResults(QAicEvent *event,
                          QAicEventCompletionType eventCompletion,
                          void *userData) {

    // Send results via queue scheduler

    if (eventCompletion == QAIC_EVENT_DEVICE_COMPLETE) {

      Payload<Sample> *p = (Payload<Sample> *)userData;

      // p->dptr->mtx_results.lock();

      // get the data from the hardware
      p->dptr->model->postprocessResults(
          &(p->samples), p->dptr->buffers_out[p->activation][p->set]);

      p->dptr->ring_buf[p->activation]->release(p);
      // p->dptr->mtx_results.unlock();
    }
  }

  virtual void DeviceInitMutex(IModel *_model, IDataSource *_data_source,
                               IConfig *_config, int hw_id,
                               std::vector<int> *aff) {

    mtx_init.lock();
    DeviceInit(_model, _data_source, _config, hw_id, aff);
    mtx_init.unlock();
  }

  std::mutex mtx_init;

  cpu_set_t cpu_affinity;

  QAicInfApi *runner;

  std::vector<RingBuffer<Sample> *> ring_buf;

  std::vector<std::vector<Sample>> samples_queue;
  std::atomic<int> sfront, sback;
  int samples_queue_depth;

  std::mutex mtx_queue;
  std::mutex mtx_ringbuf;

  int num_setup_threads;
  std::vector<Payload<Sample> *> payloads;
  std::vector<std::thread> payload_threads;

  std::thread scheduler;
  bool shim_terminate;
  std::atomic<bool> scheduler_terminate;

  QAicDeviceConfig *device_cfg;
  IModelConfig *model_cfg;

  std::mutex mtx_results;

  // activations, set, input buffers
  std::vector<std::vector<std::vector<void *>>> buffers_in;

  // activation, set, output buffers
  std::vector<std::vector<std::vector<void *>>> buffers_out;

  IModel *model;
  IDataSource *data_source;

  int scheduler_yield_time;
  int enqueue_yield_time;

  bool loop_back;
};

template <typename Sample>
IDevice<Sample> *createDevice(IModel *_model, IDataSource *_data_source,
                              IConfig *_config, int hw_id,
                              std::vector<int> aff) {
  Device<Sample> *d = new Device<Sample>();
  d->Construct(_model, _data_source, _config, hw_id, aff);
  return d;
}

#endif // DEVICE_H
