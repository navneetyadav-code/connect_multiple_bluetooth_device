using System;
using System.Runtime.InteropServices;
using System.Text;

namespace MultiAudio.Core
{
    /// <summary>
    /// P/Invoke bindings for the native MultiAudioEngine.dll.
    /// 
    /// All functions return int (HRESULT-style):
    ///   0  = success (S_OK)
    ///  &lt;0  = failure (HRESULT error code or internal error)
    ///  
    /// Exceptions:
    ///   GetDeviceCount() returns the count directly (negative = error).
    ///   RefreshDevices() returns new count (negative = error).
    /// </summary>
    public static class AudioEngine
    {
        private const string DllName = "MultiAudioEngine.dll";

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern int InitializeEngine();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern int ShutdownEngine();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern int GetDeviceCount();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true,
                   CharSet = CharSet.Unicode)]
        public static extern int GetDeviceName(int index,
            [MarshalAs(UnmanagedType.LPWStr)] StringBuilder nameBuffer, int bufferSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true,
                   CharSet = CharSet.Unicode)]
        public static extern int GetDeviceId(int index,
            [MarshalAs(UnmanagedType.LPWStr)] StringBuilder idBuffer, int bufferSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern int StartRouting();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern int StopRouting();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true,
                   CharSet = CharSet.Unicode)]
        public static extern int SetOutputEnabled(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceId, bool enabled);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true,
                   CharSet = CharSet.Unicode)]
        public static extern int SetOutputVolume(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceId, float volume);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true,
                   CharSet = CharSet.Unicode)]
        public static extern int SetOutputDelay(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceId, int delayMs);

        /// <summary>Callback invoked on an arbitrary COM thread when devices change.</summary>
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        public delegate void DeviceChangeCallback();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern void SetDeviceChangeCallback(DeviceChangeCallback callback);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern int RefreshDevices();

        /// <summary>
        /// Throw if the native call returned a failure HRESULT.
        /// </summary>
        public static void ThrowOnFailure(int hr, string operation)
        {
            if (hr < 0)
                throw new AudioEngineException(operation, hr);
        }
    }

    /// <summary>
    /// Represents a failure from the native audio engine.
    /// </summary>
    public sealed class AudioEngineException : Exception
    {
        public string Operation { get; }
        public int HResult { get; }

        public AudioEngineException(string operation, int hr)
            : base($"Native audio engine error in '{operation}': HRESULT 0x{hr:X8}")
        {
            Operation = operation;
            HResult = hr;
        }
    }
}
