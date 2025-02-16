// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_SERVICES_RECORDING_RECORDING_SERVICE_H_
#define ASH_SERVICES_RECORDING_RECORDING_SERVICE_H_

#include <memory>
#include <string>
#include <utility>

#include "ash/services/recording/public/mojom/recording_service.mojom.h"
#include "ash/services/recording/recording_encoder_muxer.h"
#include "ash/services/recording/video_capture_params.h"
#include "base/bind_post_task.h"
#include "base/callback_forward.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/sequenced_task_runner.h"
#include "base/thread_annotations.h"
#include "base/threading/sequence_bound.h"
#include "base/threading/thread_checker.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_capturer_source.h"
#include "media/base/audio_parameters.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom-forward.h"
#include "ui/gfx/image/image_skia.h"

namespace recording {

// Implements the mojo interface of the recording service which handles
// recording audio and video of the screen or portion of it, and delivers the
// webm muxed video chunks to its client.
class RecordingService : public mojom::RecordingService,
                         public viz::mojom::FrameSinkVideoConsumer,
                         public media::AudioCapturerSource::CaptureCallback {
 public:
  explicit RecordingService(
      mojo::PendingReceiver<mojom::RecordingService> receiver);
  RecordingService(const RecordingService&) = delete;
  RecordingService& operator=(const RecordingService&) = delete;
  ~RecordingService() override;

  // mojom::RecordingService:
  void RecordFullscreen(
      mojo::PendingRemote<mojom::RecordingServiceClient> client,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoCapturer> video_capturer,
      mojo::PendingRemote<media::mojom::AudioStreamFactory>
          audio_stream_factory,
      const viz::FrameSinkId& frame_sink_id,
      const gfx::Size& frame_sink_size) override;
  void RecordWindow(
      mojo::PendingRemote<mojom::RecordingServiceClient> client,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoCapturer> video_capturer,
      mojo::PendingRemote<media::mojom::AudioStreamFactory>
          audio_stream_factory,
      const viz::FrameSinkId& frame_sink_id,
      const gfx::Size& frame_sink_size,
      const viz::SubtreeCaptureId& subtree_capture_id,
      const gfx::Size& window_size) override;
  void RecordRegion(
      mojo::PendingRemote<mojom::RecordingServiceClient> client,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoCapturer> video_capturer,
      mojo::PendingRemote<media::mojom::AudioStreamFactory>
          audio_stream_factory,
      const viz::FrameSinkId& frame_sink_id,
      const gfx::Size& frame_sink_size,
      const gfx::Rect& crop_region) override;
  void StopRecording() override;
  void OnRecordedWindowChangingRoot(
      const viz::FrameSinkId& new_frame_sink_id,
      const gfx::Size& new_frame_sink_size) override;
  void OnRecordedWindowSizeChanged(const gfx::Size& new_window_size) override;
  void OnFrameSinkSizeChanged(const gfx::Size& new_frame_sink_size) override;

  // viz::mojom::FrameSinkVideoConsumer:
  void OnFrameCaptured(
      base::ReadOnlySharedMemoryRegion data,
      media::mojom::VideoFrameInfoPtr info,
      const gfx::Rect& content_rect,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks) override;
  void OnStopped() override;
  void OnLog(const std::string& message) override;

  // media::AudioCapturerSource::CaptureCallback:
  void OnCaptureStarted() override;
  void Capture(const media::AudioBus* audio_source,
               base::TimeTicks audio_capture_time,
               double volume,
               bool key_pressed) override;
  void OnCaptureError(const std::string& message) override;
  void OnCaptureMuted(bool is_muted) override;

 private:
  void StartNewRecording(
      mojo::PendingRemote<mojom::RecordingServiceClient> client,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoCapturer> video_capturer,
      mojo::PendingRemote<media::mojom::AudioStreamFactory>
          audio_stream_factory,
      std::unique_ptr<VideoCaptureParams> capture_params);

  // Called on the main thread during an on-going recording to reconfigure an
  // existing video encoder.
  void ReconfigureVideoEncoder();

  // Called on the main thread on |success| from OnStopped() when all video
  // frames have been sent, or from OnEncodingFailure() with |success| set to
  // false.
  void TerminateRecording(bool success);

  // Binds the given |video_capturer| to |video_capturer_remote_| and starts
  // video according to the current |current_video_capture_params_|.
  void ConnectAndStartVideoCapturer(
      mojo::PendingRemote<viz::mojom::FrameSinkVideoCapturer> video_capturer);

  // If the video capturer gets disconnected (e.g. Viz crashes) during an
  // ongoing recording, this attempts to reconnect to a new capturer and resumes
  // capturing with the same |current_video_capture_params_|.
  void OnVideoCapturerDisconnected();

  // The captured audio data is delivered to Capture() on a dedicated thread
  // created by the audio capturer. However, we can only schedule tasks on the
  // |encoder_muxer_| using its |base::SequenceBound| wrapper on the main
  // thread. This function is called on the main thread, and is scheduled using
  // |main_task_runner_| from Capture().
  void OnAudioCaptured(std::unique_ptr<media::AudioBus> audio_bus,
                       base::TimeTicks audio_capture_time);

  // This is called by |encoder_muxer_| on the main thread (since we bound it as
  // a callback to be invoked on the main thread. See BindOnceToMainThread()),
  // when a failure of |type| occurs during audio or video encoding depending on
  // |for_video|. This ends the ongoing recording and signals to the client that
  // a failure occurred.
  void OnEncodingFailure(FailureType type, bool for_video);

  // Called back on the main thread to terminate the recording immediately as a
  // result of a failure.
  void OnRecordingFailure();

  // At the end of recording we ask the |encoder_muxer_| to flush and process
  // any buffered frames. When this completes this function is called on the
  // main thread (since it's bound as a callback to be invoked on the main
  // thread. See BindOnceToMainThread()). |success| indicates whether we're
  // flushing due to normal recording termination, or due to a failure.
  void OnEncoderMuxerFlushed(bool success);

  // Called on the main thread to send the given |muxer_output| chunk to the
  // client of this service.
  void SignalMuxerOutputToClient(std::string muxer_output);

  // Called on the main thread to tell the client that recording ended. No more
  // chunks will be sent to the client after this.
  void SignalRecordingEndedToClient(bool success);

  // Bound as a callback to be repeatedly invoked on the |main_task_runner_|.
  // It is called by |encoder_muxer_| to deliver a muxer output chunk |data|.
  void OnMuxerOutput(std::string data);

  // Called on the main thread to flush the chunks buffered in
  // |muxed_chunks_buffer_| by sending them to the client and reset the
  // |number_of_buffered_chunks_| back to 0.
  void FlushBufferedChunks();

  // By default, the |encoder_muxer_| will invoke any callback we provide it
  // with to notify us of certain events (such as muxer output, or flush done)
  // on the |encoding_task_runner_|'s sequence. But since these callbacks are
  // invoked asynchronously from other threads, they may get invoked after this
  // RecordingService instance had been destroyed. Therefore, we need to bind
  // these callbacks to weak ptrs, to prevent them from invoking after this
  // object's destruction. However, this won't work, since weak ptrs cannot be
  // invalidated except on the sequence on which they were invoked on. Hence, we
  // must make sure these callbacks are invoked on the main thread.
  //
  // The below are two convenience methods to bind once and repeating callbacks
  // to weak ptrs that would only be invoked on the main thread.
  template <typename Functor, typename... Args>
  auto BindOnceToMainThread(Functor&& functor, Args&&... args) {
    return base::BindPostTask(main_task_runner_,
                              base::BindOnce(std::forward<Functor>(functor),
                                             weak_ptr_factory_.GetWeakPtr(),
                                             std::forward<Args>(args)...));
  }
  template <typename Functor, typename... Args>
  auto BindRepeatingToMainThread(Functor&& functor, Args&&... args) {
    return base::BindPostTask(
        main_task_runner_, base::BindRepeating(std::forward<Functor>(functor),
                                               weak_ptr_factory_.GetWeakPtr(),
                                               std::forward<Args>(args)...));
  }

  THREAD_CHECKER(main_thread_checker_);

  // The audio parameters that will be used when recording audio.
  const media::AudioParameters audio_parameters_;

  // The mojo receiving end of the service.
  mojo::Receiver<mojom::RecordingService> receiver_;

  // The mojo receiving end of the service as a FrameSinkVideoConsumer.
  mojo::Receiver<viz::mojom::FrameSinkVideoConsumer> consumer_receiver_
      GUARDED_BY_CONTEXT(main_thread_checker_);

  // A task runner to post tasks on the main thread of the recording service.
  // It can be accessed from any thread.
  scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;

  // A sequenced blocking pool task runner used to run all encoding and muxing
  // tasks on. Can be accessed from any thread.
  scoped_refptr<base::SequencedTaskRunner> encoding_task_runner_;

  // A mojo remote end of client of this service (e.g. Ash). There can only be
  // a single client of this service.
  mojo::Remote<mojom::RecordingServiceClient> client_remote_
      GUARDED_BY_CONTEXT(main_thread_checker_);

  // A cached scaled down rgb image of the first valid video frame which will be
  // used to provide the client with an image thumbnail representing the
  // recorded video.
  gfx::ImageSkia video_thumbnail_ GUARDED_BY_CONTEXT(main_thread_checker_);

  // True if a failure has been propagated from |encoder_muxer_| that we will
  // end recording abruptly and ignore any incoming audio/video frames.
  bool did_failure_occur_ GUARDED_BY_CONTEXT(main_thread_checker_) = false;

  // The parameters of the current ongoing video capture. This object knows how
  // to initialize the video capturer depending on which capture source
  // (fullscreen, window, or region) is currently being recorded. It is set to
  // a nullptr when there's no recording happening.
  std::unique_ptr<VideoCaptureParams> current_video_capture_params_
      GUARDED_BY_CONTEXT(main_thread_checker_);

  // The mojo remote end used to interact with a video capturer living on Viz.
  mojo::Remote<viz::mojom::FrameSinkVideoCapturer> video_capturer_remote_
      GUARDED_BY_CONTEXT(main_thread_checker_);

  // The audio capturer instance. It is created only if the service is requested
  // to record audio along side the video.
  scoped_refptr<media::AudioCapturerSource> audio_capturer_
      GUARDED_BY_CONTEXT(main_thread_checker_);

  // Performs all encoding and muxing operations asynchronously on the
  // |encoding_task_runner_|. However, the |encoder_muxer_| object itself is
  // constructed, used, and destroyed on the main thread sequence.
  base::SequenceBound<RecordingEncoderMuxer> encoder_muxer_
      GUARDED_BY_CONTEXT(main_thread_checker_);

  // To avoid doing a ton of IPC calls to the client for each muxed chunk
  // received from |encoder_muxer_| in OnMuxerOutput(), we buffer those chunks
  // here up to |kMaxBufferedChunks| before we send them to the client over IPC
  // in SignalMuxerOutputToClient().
  std::string muxed_chunks_buffer_ GUARDED_BY_CONTEXT(main_thread_checker_);
  int number_of_buffered_chunks_ GUARDED_BY_CONTEXT(main_thread_checker_) = 0;

  base::WeakPtrFactory<RecordingService> weak_ptr_factory_{this};
};

}  // namespace recording
#endif  // ASH_SERVICES_RECORDING_RECORDING_SERVICE_H_
