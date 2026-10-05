using System;
using System.Runtime.InteropServices;
using System.Text;

namespace MultiAudio.Core
{
    public static class AudioEngine
    {
        private const string DllName = "MultiAudioEngine.dll";

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void InitializeEngine();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ShutdownEngine();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int GetDeviceCount();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern void GetDeviceName(int index, StringBuilder nameBuffer, int bufferSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern void GetDeviceId(int index, StringBuilder idBuffer, int bufferSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void StartRouting();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void StopRouting();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern void SetOutputEnabled(string deviceId, bool enabled);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern void SetOutputVolume(string deviceId, float volume);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern void SetOutputDelay(string deviceId, int delayMs);
    }
}
