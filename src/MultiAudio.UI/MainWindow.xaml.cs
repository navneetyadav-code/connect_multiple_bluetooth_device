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

        public MainWindow()
        {
            InitializeComponent();
            DataContext = this;
            DevicesList.ItemsSource = Devices;
            
            LoadDevices();
        }

        private void LoadDevices()
        {
            try
            {
                AudioEngine.InitializeEngine();
                int count = AudioEngine.GetDeviceCount();
                
                for (int i = 0; i < count; i++)
                {
                    StringBuilder nameBuffer = new StringBuilder(256);
                    AudioEngine.GetDeviceName(i, nameBuffer, 256);
                    
                    StringBuilder idBuffer = new StringBuilder(256);
                    AudioEngine.GetDeviceId(i, idBuffer, 256);

                    Devices.Add(new AudioDeviceModel
                    {
                        Id = idBuffer.ToString(),
                        Name = nameBuffer.ToString(),
                        VolumeText = "🔊 100%",
                        DelayText = "Delay 0ms",
                        IsEnabled = false
                    });
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to load devices: {ex.Message}");
            }
        }

        private void StartRoutingButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                AudioEngine.StartCapture();
                foreach (var device in Devices)
                {
                    if (device.IsEnabled)
                    {
                        AudioEngine.AddOutputDevice(device.Id);
                    }
                }
                
                StartRoutingButton.IsEnabled = false;
                StopRoutingButton.IsEnabled = true;
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to start routing: {ex.Message}");
            }
        }

        private void StopRoutingButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                AudioEngine.StopCapture();
                foreach (var device in Devices)
                {
                    if (device.IsEnabled)
                    {
                        AudioEngine.RemoveOutputDevice(device.Id);
                    }
                }
                
                StartRoutingButton.IsEnabled = true;
                StopRoutingButton.IsEnabled = false;
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to stop routing: {ex.Message}");
            }
        }

        protected override void OnClosed(EventArgs e)
        {
            AudioEngine.ShutdownEngine();
            base.OnClosed(e);
        }
    }

    public class AudioDeviceModel
    {
        public string Id { get; set; } = "";
        public string Name { get; set; } = "";
        public string VolumeText { get; set; } = "";
        public string DelayText { get; set; } = "";
        public bool IsEnabled { get; set; }
    }
}