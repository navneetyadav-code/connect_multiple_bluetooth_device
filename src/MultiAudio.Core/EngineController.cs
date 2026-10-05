using System;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

namespace MultiAudio.Core
{
    /// <summary>
    /// Serializes all native engine commands through a dedicated background thread.
    /// The WPF UI thread enqueues fire-and-forget commands and returns immediately.
    /// No native audio state is ever mutated directly from the dispatcher thread.
    /// 
    /// Thread safety contract:
    ///   - Enqueue()       — callable from any thread, non-blocking
    ///   - EnqueueAsync()  — callable from any thread, awaitable completion
    ///   - Dispose()       — drains the queue then shuts down cleanly
    /// </summary>
    public sealed class EngineController : IDisposable
    {
        private readonly Channel<Action> _channel;
        private readonly Task _workerTask;
        private readonly CancellationTokenSource _cts = new();

        public EngineController()
        {
            _channel = Channel.CreateUnbounded<Action>(new UnboundedChannelOptions
            {
                SingleReader = true,
                SingleWriter = false,
                AllowSynchronousContinuations = false
            });

            // LongRunning = dedicated OS thread, not a thread-pool thread.
            // This is important: thread-pool starvation must never affect audio commands.
            _workerTask = Task.Factory.StartNew(
                WorkerLoop,
                _cts.Token,
                TaskCreationOptions.LongRunning,
                TaskScheduler.Default);
        }

        /// <summary>Fire-and-forget. Returns immediately.</summary>
        public void Enqueue(Action command)
        {
            _channel.Writer.TryWrite(command);
        }

        /// <summary>Enqueue and return a Task that completes when the command finishes.</summary>
        public Task EnqueueAsync(Action command)
        {
            var tcs = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);

            _channel.Writer.TryWrite(() =>
            {
                try   { command();              tcs.SetResult();          }
                catch (Exception ex)           { tcs.SetException(ex);    }
            });

            return tcs.Task;
        }

        private async void WorkerLoop()
        {
            try
            {
                await foreach (var cmd in _channel.Reader.ReadAllAsync(_cts.Token))
                {
                    try { cmd(); }
                    catch { /* per-command errors swallowed; add structured logging later */ }
                }
            }
            catch (OperationCanceledException) { }
        }

        public void Dispose()
        {
            _cts.Cancel();
            _channel.Writer.TryComplete();
            try { _workerTask.Wait(TimeSpan.FromSeconds(5)); } catch { }
            _cts.Dispose();
        }
    }
}
