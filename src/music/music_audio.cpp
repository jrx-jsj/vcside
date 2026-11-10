// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "music_audio.h"

#include <SDL3/SDL.h>

namespace vcs::music {

static constexpr int kRate = 48000;

MusicAudio::~MusicAudio() { close(); }

void MusicAudio::close() {
  // destroying the stream stops the device and its thread first; after SDL_Quit
  // there is nothing left to destroy
  if (stream_ && SDL_WasInit(SDL_INIT_AUDIO)) SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
  failed_ = true;  // no reopening while the app closes
}

bool MusicAudio::ready(std::string* error) {
  if (stream_) return true;
  if (!failed_) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      error_ = SDL_GetError();
      failed_ = true;
    } else {
      SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kRate};
      stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, this);
      if (!stream_) error_ = SDL_GetError(), failed_ = true;
      else SDL_ResumeAudioStreamDevice(stream_);
    }
  }
  if (error) *error = error_;
  return stream_ != nullptr;
}

void MusicAudio::use(const std::string& tv) {
  if (tv == tv_) return;
  tv_ = tv;
  renderer_.setup(tv, kRate);
}

void MusicAudio::play(const Song& song, const std::string& tv, SongPosition from) {
  if (!ready(nullptr)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  use(tv);
  renderer_.player.start(song, audioClockHz(tv), from);
}

void MusicAudio::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  renderer_.player.stop();
}

void MusicAudio::updateSong(const Song& song) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (renderer_.player.playing()) renderer_.player.updateSong(song);
}

void MusicAudio::noteOn(const Song& song, const std::string& tv, int channel, int note, int instrument) {
  if (!ready(nullptr)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  use(tv);
  renderer_.player.noteOn(song, audioClockHz(tv), channel, note, instrument);
}

void MusicAudio::playEffect(const Song& song, const std::string& tv, int effect) {
  if (!ready(nullptr)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  use(tv);
  renderer_.player.playEffect(song, audioClockHz(tv), effect);
}

void MusicAudio::noteOff(int channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  renderer_.player.noteOff(channel);
}

void MusicAudio::setMuted(int channel, bool muted) {
  std::lock_guard<std::mutex> lock(mutex_);
  renderer_.muted[channel] = muted;
}

bool MusicAudio::playing() {
  std::lock_guard<std::mutex> lock(mutex_);
  return renderer_.player.playing();
}

SongPosition MusicAudio::position() {
  std::lock_guard<std::mutex> lock(mutex_);
  return renderer_.player.position();
}

FrameRegisters MusicAudio::registers() {
  std::lock_guard<std::mutex> lock(mutex_);
  return renderer_.lastRegisters();
}

void MusicAudio::fill(int bytesWanted) {
  std::lock_guard<std::mutex> lock(mutex_);
  buffer_.clear();
  // whole video frames, as the console plays them; silence when nothing sounds
  while ((int)(buffer_.size() * sizeof(float)) < bytesWanted) {
    if (tv_.empty()) use("NTSC");
    renderer_.renderFrame(buffer_);
  }
}

void MusicAudio::feed(void* userData, SDL_AudioStream* stream, int additionalBytes, int) {
  if (additionalBytes <= 0) return;
  auto* audio = static_cast<MusicAudio*>(userData);
  audio->fill(additionalBytes);
  SDL_PutAudioStreamData(stream, audio->buffer_.data(), (int)(audio->buffer_.size() * sizeof(float)));
}

}  // namespace vcs::music
