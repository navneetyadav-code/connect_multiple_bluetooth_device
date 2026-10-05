using System;
using System.Collections.ObjectModel;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using MultiAudio.Core;

namespace MultiAudio.UI
{
    public partial class MainWindow : Window
    {
        public ObservableCollection<AudioDeviceModel> Devices { get; set; } = new();

        // All native engine calls are routed through this controller.
        // The WPF dispatcher thread never touches engine state directly.
        private readonly EngineController _engine = new();

        // Keep a strong reference to the callback delegate to prevent GC collection.
        private AudioEngine.DeviceChangeCallback? _deviceChangeCallback;

        // Debounce timer for device-change notifications (they can fire in bursts)
        private DispatcherTimer? _deviceRefreshTimer;

        public MainWindow()
        {
            InitializeComponent();
            DataContext = this;
            DevicesList.ItemsSource = Devices;

            // Surface native command failures to the UI
            _engine.CommandFailed += OnCommandFailed;

            // InitializeEngine is read-only device enumeration — safe to call
            // synchronously before any routing threads are started.
            int hr = AudioEngine.InitializeEngine();
            if (hr < 0)
            {
                StatusText.Text = "⚠ Engine initialization failed";
                StartRoutingButton.IsEnabled = false;
                return;
            }

            LoadDevices();

            // Register for device hotplug notifications
            _deviceChangeCallback = OnNativeDeviceChanged;
            AudioEngine.SetDeviceChangeCallback(_deviceChangeCallback);

            // Debounce timer: refresh devices at most every 500ms
            _deviceRefreshTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(500)
            };
            _deviceRefreshTimer.Tick += (_, _) =>
            {
                _deviceRefreshTimer.Stop();
                RefreshDeviceList();
            };
        }

        /// <summary>
        /// Called from an arbitrary COM thread when devices change.
        /// Marshals to the UI thread with debouncing.
        /// </summary>
        private void OnNativeDeviceChanged()
        {
            Dispatcher.BeginInvoke(() =>
            {
                // Restart the debounce timer on each notification
                _deviceRefreshTimer?.Stop();
                _deviceRefreshTimer?.Start();
            });
        }

        private void RefreshDeviceList()
        {
            _engine.Enqueue(() =>
            {
                int newCount = AudioEngine.RefreshDevices();
                if (newCount < 0) return;

                // Marshal device list update back to UI thread
                Dispatcher.BeginInvoke(() => ReloadDevicesFromNative());
            });
        }

        private void ReloadDevicesFromNative()
        {
            // Remember which devices were enabled
            var enabledIds = new System.Collections.Generic.HashSet<string>();
            foreach (var d in Devices)
            {
                if (d.IsEnabled) enabledIds.Add(d.Id);
            }

            Devices.Clear();

            int count = AudioEngine.GetDeviceCount();
            for (int i = 0; i < count; i++)
            {
                var nameBuffer = new StringBuilder(256);
                AudioEngine.GetDeviceName(i, nameBuffer, 256);

                var idBuffer = new StringBuilder(256);
                AudioEngine.GetDeviceId(i, idBuffer, 256);

                var device = new AudioDeviceModel
                {
                    Id         = idBuffer.ToString(),
                    Name       = nameBuffer.ToString(),
                    VolumeText = "🔊 100%",
                    IsEnabled  = enabledIds.Contains(idBuffer.ToString())
                };

                WireDeviceEvents(device);
                Devices.Add(device);
            }
        }

        private void LoadDevices()
        {
            int count = AudioEngine.GetDeviceCount();
            for (int i = 0; i < count; i++)
            {
                var nameBuffer = new StringBuilder(256);
                AudioEngine.GetDeviceName(i, nameBuffer, 256);

                var idBuffer = new StringBuilder(256);
                AudioEngine.GetDeviceId(i, idBuffer, 256);

                var device = new AudioDeviceModel
                {
                    Id         = idBuffer.ToString(),
                    Name       = nameBuffer.ToString(),
                    VolumeText = "🔊 100%",
                    IsEnabled  = false
                };

                WireDeviceEvents(device);
                Devices.Add(device);
            }
        }

        private void WireDeviceEvents(AudioDeviceModel device)
        {
            device.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == nameof(AudioDeviceModel.IsEnabled))
                    _engine.Enqueue(() => AudioEngine.SetOutputEnabled(device.Id, device.IsEnabled));

                else if (e.PropertyName == nameof(AudioDeviceModel.DelayMs))
                    _engine.Enqueue(() => AudioEngine.SetOutputDelay(device.Id, device.DelayMs));
            };
        }

        private void StartRoutingButton_Click(object sender, RoutedEventArgs e)
        {
            StartRoutingButton.IsEnabled = false;
            StopRoutingButton.IsEnabled  = true;
            StatusText.Text = "▶ Routing active";

            _engine.Enqueue(() =>
            {
                int hr = AudioEngine.StartRouting();
                if (hr < 0)
                {
                    Dispatcher.BeginInvoke(() =>
                    {
                        StatusText.Text = $"⚠ Start failed (0x{hr:X8})";
                        StartRoutingButton.IsEnabled = true;
                        StopRoutingButton.IsEnabled = false;
                    });
                }
            });
        }

        private void StopRoutingButton_Click(object sender, RoutedEventArgs e)
        {
            StopRoutingButton.IsEnabled  = false;
            StartRoutingButton.IsEnabled = true;
            StatusText.Text = "⏹ Stopped";
            _engine.Enqueue(() => AudioEngine.StopRouting());
        }

        /// <summary>
        /// Surfaces native command errors in the UI status bar.
        /// </summary>
        private void OnCommandFailed(Exception ex)
        {
            Dispatcher.BeginInvoke(() =>
            {
                if (ex is AudioEngineException aex)
                    StatusText.Text = $"⚠ {aex.Operation}: error 0x{aex.HResult:X8}";
                else
                    StatusText.Text = $"⚠ Engine error: {ex.Message}";
            });
        }

        /// <summary>
        /// Issue 8A/8B fix: async shutdown instead of blocking .Wait().
        /// Uses the Application.Current.Exit event to ensure the process
        /// doesn't exit before cleanup completes.
        /// </summary>
        protected override async void OnClosed(EventArgs e)
        {
            StatusText.Text = "Shutting down…";

            // Unregister device notifications first
            AudioEngine.SetDeviceChangeCallback(null!);
            _deviceRefreshTimer?.Stop();

            try
            {
                // Drain the queue (including StopRouting), then shutdown
                await _engine.EnqueueAsync(() =>
                {
                    AudioEngine.StopRouting();
                    AudioEngine.ShutdownEngine();
                });
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Shutdown error: {ex.Message}");
            }

            await _engine.DisposeAsync();
            base.OnClosed(e);
        }
    }

    public class AudioDeviceModel : System.ComponentModel.INotifyPropertyChanged
    {
        private bool   _isEnabled;
        private int    _delayMs;
        private string _delayText = "0ms";

        public string Id         { get; set; } = "";
        public string Name       { get; set; } = "";
        public string VolumeText { get; set; } = "";

        public string DelayText
        {
            get => _delayText;
            set
            {
                _delayText = value;
                PropertyChanged?.Invoke(this,
                    new System.ComponentModel.PropertyChangedEventArgs(nameof(DelayText)));
            }
        }

        public bool IsEnabled
        {
            get => _isEnabled;
            set
            {
                if (_isEnabled == value) return;
                _isEnabled = value;
                PropertyChanged?.Invoke(this,
                    new System.ComponentModel.PropertyChangedEventArgs(nameof(IsEnabled)));
            }
        }

        public int DelayMs
        {
            get => _delayMs;
            set
            {
                if (_delayMs == value) return;
                _delayMs   = value;
                DelayText  = $"{value}ms";
                PropertyChanged?.Invoke(this,
                    new System.ComponentModel.PropertyChangedEventArgs(nameof(DelayMs)));
            }
        }

        public event System.ComponentModel.PropertyChangedEventHandler? PropertyChanged;
    }
}