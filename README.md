# MultiAudio (MVP)

A Windows desktop application that takes the system's audio, creates a virtual/mixed audio path, and sends synchronized copies to multiple physical outputs simultaneously.

## Features (Phase 1)
- Capture Windows system audio (WASAPI Loopback)
- Enumerate available audio devices
- Select multiple outputs (Bluetooth, USB, 3.5mm, etc.)
- C# / WPF User Interface
- Native C++ Audio Engine

## Architecture

The application is structured into the following components:
- **MultiAudio.UI**: WPF frontend for managing devices and routing.
- **MultiAudio.Core**: C# wrapper for the native audio engine (P/Invoke).
- **MultiAudio.AudioEngine**: C++ native library using MMDevice and WASAPI APIs.

## Next Steps
- Implement WASAPI Loopback Capture in the native engine.
- Implement resampling and audio mixing for multiple outputs.
- Implement delay/synchronization buffers.
- Enhance UI with profiles and settings.

## Build Requirements
- .NET 10.0 SDK
- Visual Studio 2022 (C++ Desktop Development)
