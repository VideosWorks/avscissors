/*
 * Tarpeeksi Hyvae Soft 2018 /
 * AV Scissors.
 *
 * Looks through the given video's frame and audio data to find which of the video's
 * frames contain activity.
 *
 * Activity here is defined as either a sound appreciably above the noise baseline,
 * or significant-enough differences between two subsequent frames of video to suggest
 * movement in them.
 *
 */

#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QFuture>
#include <QDebug>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "../../src/video/video_activity.h"
#include "../../src/messager/messager.h"
#include "../../src/video/video_info.h"
#include "../../src/audio/audio_file.h"
#include "../../src/common.h"

/// Temp hack. Will adjust the temporal size of activity slices; i.e. the number of
/// subsequent frames that activity in one frame will cause to be marked as active,
/// as well. This is mainly done as a usability feature, so that active areas on the
/// GUI's activity strips are easier to click on, rather than being just a few pixels
/// wide for when only brief activity occurs.
static const uint TIME_GRANULARITY_DIVISOR = 50;

video_activity_c::video_activity_c(const video_info_c &sourceVideo, const messager_c *const messager) :
    messager(messager),
    videoInfo(sourceVideo)
{
    connect(    this, &video_activity_c::message_to_user,
            messager, &messager_c::new_message);

    if (!sourceVideo.is_valid_video())
    {
        return;
    }

    // Pre-reserve room for the frame activity data.
    {
        k_assert((this->videoInfo.num_frames() > 0), "The video contains no frames.");

        this->videoFrameIsActive.resize(this->videoInfo.num_frames());
        this->audioFrameIsActive.resize(this->videoInfo.num_frames());

        qFill(this->videoFrameIsActive, activity_type_e::Uninitialized);
        qFill(this->audioFrameIsActive, activity_type_e::Uninitialized);
    }

    // Start processing the video's activity in separate worker threads.
    workerThreadsShouldStop = false;
    this->videoStripThread = QtConcurrent::run(this, &video_activity_c::mark_video_frame_activity);
    this->audioStripThread = QtConcurrent::run(this, &video_activity_c::mark_audio_frame_activity);

    return;
}

video_activity_c::~video_activity_c()
{
    workerThreadsShouldStop = true;
    this->videoStripThread.waitForFinished();
    this->audioStripThread.waitForFinished();

    delete audio;

    return;
}

bool video_activity_c::is_active_frame_at(const uint offs, const uint videoOrAudioOrBoth) const
{
    switch (videoOrAudioOrBoth)
    {
    case 0: return bool(this->videoFrameIsActive.at(offs) == activity_type_e::Active);
    case 1: return bool(this->audioFrameIsActive.at(offs) == activity_type_e::Active);
    case 2: return bool((this->audioFrameIsActive.at(offs) == activity_type_e::Active) ||
                        (this->videoFrameIsActive.at(offs) == activity_type_e::Active));
    default: k_assert(0, "Unknown track type."); return false;
    }
}

bool video_activity_c::has_valid_audio() const
{
    return bool((this->audio != nullptr) &&
                this->audio->has_valid_audio_data());
}

// Returns true once the video and audio activity strips have finished
// processing.
//
bool video_activity_c::strip_build_has_finished() const
{
    return bool(videoStripThread.isFinished() && audioStripThread.isFinished());
}

// Assumes that the given frame is active; iterates backwards from it to find the
// frame in which that activity began.
//
uint video_activity_c::get_start_of_active_segment(const uint startFrameIdx, const uint videoOrAudio) const
{
    uint closest = startFrameIdx;
    const auto &frameActivity = ((videoOrAudio == 0)? this->videoFrameIsActive
                                                    : this->audioFrameIsActive);

    while (frameActivity.at(closest) == activity_type_e::Active)
    {
        closest--;
        if (closest == 0) // If reached the end.
        {
            return 0;
        }
    }

    return (closest + 1);
}

// Calls FFMPEG as an external process to extract the video's audio into an easier-
// to-process WAV file. This is purely for programmer convenience.
//
void video_activity_c::extract_audio(void)
{
    const QString audioFilename = (this->videoInfo.file_name() + ".wav");

    // Run FFMPEG to extract the audio file. Assumes that the user already has
    // FFMPEG available on their system, and that it's callable globally. If not,
    // no audio activity information will be available.
    {
        QString cmd = QString("ffmpeg -i \"%1\" -flags bitexact -map_metadata -1 -acodec pcm_s16le -ac 1 -y \"%2\"")
                      .arg(this->videoInfo.file_name())
                      .arg(audioFilename);

        const int ret = system(cmd.toStdString().c_str());
        if (ret != 0)
        {
            NBENE(("Failed to extract the video's audio using FFMPEG. Audio information will not be available."));
            emit message_to_user("The audio track could not be processed.");
            return;
        }
    }

    // Load the audio file's data into its own object, which we can then process
    // for activity, later.
    this->audio = new audio_file_c(audioFilename, this->messager);

    // We can delete the temporary audio file from disk now.
    QFile(audioFilename).remove();

    return;
}

// Works through all the audio samples in the video's (extracted) audio track
// to find ones whose amplitude is notably above the baseline. The corresponding
// frames in the video will be marked as having acoustic activity.
//
/// TODO. Cleanup.
void video_activity_c::mark_audio_frame_activity(void)
{
    const uint timeGranularity = (this->videoInfo.num_frames() / TIME_GRANULARITY_DIVISOR);

    this->extract_audio();
    if (!this->has_valid_audio())
    {
        qFill(this->audioFrameIsActive, activity_type_e::NoData);
        return;
    }

    k_assert(this->has_valid_audio(), "Was asked to mark audio activity, but the audio was not valid.")
    k_assert((this->videoInfo.num_frames() > 0), "Asked to mark audio activity, but there are no frames to mark it for.");

    // Get the average and maximum amplitudes in the audio's samples.
    real avgAmplitude = 0;
    uint maxAmplitude = 0;
    {
        i64 sumAmplitude = 0;

        for (uint i = 0; i < this->audio->num_samples(); i++)
        {
            const auto &sample = this->audio->sample_at(i);

            sumAmplitude += sample;

            if (abs(sample) > maxAmplitude)
            {
                maxAmplitude = abs(sample);
            }
        }

        avgAmplitude = (sumAmplitude / (real)this->audio->num_samples());
    }

    // Mark the frames based on whether audio corresponding to each frame is
    // exceeding the average amplitude across the whole audio track.
    /// Temp hack. Could instead use quartiles or something.
    const real thresholdAmplitude = ((maxAmplitude - avgAmplitude) * 0.001);
    {
        for (uint i = 0; i < this->videoInfo.num_frames(); i++)
        {
            const uint sampleOffs = ((this->audio->num_samples() / (real)this->videoInfo.num_frames()) * i);
            const bool loudSample = bool(fabs(this->audio->sample_at(sampleOffs)) > fabs(thresholdAmplitude));

            this->audioFrameIsActive[i] = loudSample? activity_type_e::Active
                                                    : activity_type_e::Inactive;

            if (loudSample)
            {
                const uint numFramesToSkip = ((i + timeGranularity) > this->videoInfo.num_frames())? (this->videoInfo.num_frames() - i)
                                                                                                   : timeGranularity;

                for (uint p = 0; p < numFramesToSkip; (p++, i++))
                {
                    this->audioFrameIsActive[i] = activity_type_e::Active;
                }
            }

            // Periodically check to make sure the user doesn't want us to stop processing.
            if (((i % 200) == 0) &&
                this->workerThreadsShouldStop)
            {
                return;
            }
        }
    }

    return;
}

bool video_activity_c::frames_differ(const cv::Mat &frame1, const cv::Mat &frame2,
                                     const u8 threshold)
{
    k_assert(((frame1.rows == frame2.rows) && (frame1.cols == frame2.cols)),
             "Frame sizes do not match.");

    cv::Mat difference = (frame1 - frame2);

    for (int y = 0; y < frame1.rows; y++)
    {
        for (int x = 0; x < frame1.cols; x++)
        {
            const auto diffPixel = difference.at<cv::Vec3b>(y, x);

            if ((abs(diffPixel[0]) > threshold) ||
                (abs(diffPixel[1]) > threshold) ||
                (abs(diffPixel[2]) > threshold))
            {
                return true;
            }
        }
    }

    return false;
}

// Compares the video's frames in pairs, and marks a given frame as active if
// its color values differ notably from those of the preceding frame.
//
void video_activity_c::mark_video_frame_activity(void)
{
    const uint timeGranularity = (this->videoInfo.num_frames() / TIME_GRANULARITY_DIVISOR);

    cv::VideoCapture video(this->videoInfo.file_name().toStdString());
    k_assert(video.isOpened(), "Failed to open the video file in OpenCV.");

    // Compare each frame in the video to the previous one to find which segments
    // of the video contain no activity, i.e. between which no single pixel varies
    // by more than the allowed threshold.
    {
        cv::Mat thisFrame, prevFrame;

        video.set(CV_CAP_PROP_POS_FRAMES, 0);
        video >> thisFrame;
        this->videoFrameIsActive[0] = activity_type_e::Inactive;

        for (uint i = 1; i < this->videoInfo.num_frames(); i++)
        {
            prevFrame = thisFrame.clone();
            video >> thisFrame;

            k_assert((thisFrame.channels() == 3),
                     "Expected three colors channels in the video frame.");
            k_assert((thisFrame.channels() == prevFrame.channels()),
                     "Found mismatched frames while reading the video.");
            k_assert((thisFrame.total() == size_t(this->videoInfo.width() * this->videoInfo.height())),
                     "Encountered a frame with an unexpected size.");
            k_assert((thisFrame.total() == prevFrame.total()),
                     "Found mismatched frames while reading the video.");

            this->videoFrameIsActive[i] = frames_differ(thisFrame, prevFrame, 30)? activity_type_e::Active
                                                                                 : activity_type_e::Inactive;

            // If we get an active frame, assume (for performance reasons) that the
            // next x frames will also contain activity, so skip through them.
            if (this->videoFrameIsActive[i] == activity_type_e::Active)
            {
                const uint numFramesToSkip = ((i + timeGranularity) > this->videoInfo.num_frames())? (this->videoInfo.num_frames() - i)
                                                                                                   : timeGranularity;

                for (uint p = 0; p < numFramesToSkip; (p++, i++))
                {
                    this->videoFrameIsActive[i] = activity_type_e::Active;
                }

                // Seek to the next frame we want to capture, and grab it, so that it
                // becomes the previous frame on the next iteration of the loop.
                this->videoFrameIsActive[i] = activity_type_e::Inactive;
                video.set(CV_CAP_PROP_POS_FRAMES, i);
                video >> thisFrame;
            }

            // Periodically check to make sure the user doesn't want us to stop processing.
            if (((i % 200) == 0) &&
                this->workerThreadsShouldStop)
            {
                return;
            }
        }
    }

    k_assert(uint(this->videoFrameIsActive.size()) == this->videoInfo.num_frames(), "Some frames were skipped while marking activity.");

    return;
}
