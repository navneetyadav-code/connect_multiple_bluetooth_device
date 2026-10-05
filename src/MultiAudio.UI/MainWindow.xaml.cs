using System;
using System.Collections.ObjectModel;
using System.Text;
using System.Windows;
using MultiAudio.Core;

namespace MultiAudio.UI
{
    public partial class MainWindow : Window
    {
        public ObservableCollection<AudioDeviceModel> Devices { get; set; } = new();

        // Issue #7 Fix: all native engine calls are routed through this controller.
        // The WPF dispatcher thread never touches engine state directly.
        private readonly EngineController _engine = new();

        public MainWindow()
        {
            InitializeComponent();
            DataContext = this;
            DevicesList.ItemsSource = Devices;

            // InitializeEngine is read-only device enumeration — safe to call
            // synchronously before any routing threads are started.
            AudioEngine.InitializeEngine();
            LoadDevices();
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

                // Wire property changes — all go through the command queue, never direct
                device.PropertyChanged += (s, e) =>
                {
                    if (e.PropertyName == nameof(AudioDeviceModel.IsEnabled))
                        _engine.Enqueue(() => AudioEngine.SetOutputEnabled(device.Id, device.IsEnabled));

                    else if (e.PropertyName == nameof(AudioDeviceModel.DelayMs))
                        _engine.Enqueue(() => AudioEngine.SetOutputDelay(device.Id, device.DelayMs));
                };

                Devices.Add(device);
            }
        }

        private void StartRoutingButton_Click(object sender, RoutedEventArgs e)
        {
            // Disable button immediately on the UI thread; the engine command is async
            StartRoutingButton.IsEnabled = false;
            StopRoutingButton.IsEnabled  = true;
            _engine.Enqueue(AudioEngine.StartRouting);
        }

        private void StopRoutingButton_Click(object sender, RoutedEventArgs e)
        {
            StopRoutingButton.IsEnabled  = false;
            StartRoutingButton.IsEnabled = true;
            _engine.Enqueue(AudioEngine.StopRouting);
        }

        protected override void OnClosed(EventArgs e)
        {
            // Wait for StopRouting + ShutdownEngine to complete before the process exits
            _engine.EnqueueAsync(AudioEngine.StopRouting)
                   .ContinueWith(_ => AudioEngine.ShutdownEngine())
                   .Wait(TimeSpan.FromSeconds(5));
            _engine.Dispose();
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