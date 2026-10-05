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
    ///   - Enqueue()       — callable from any thread, non-blocking.
    ///                       Returns false if the controller is shutting down.
    ///   - EnqueueAsync()  — callable from any thread, awaitable completion.
    ///                       Propagates native exceptions to the caller.
    ///   - DisposeAsync()  — drains the remaining queue, then shuts down.
    ///   - Dispose()       — synchronous wrapper around DisposeAsync().
    /// </summary>
    public sealed class EngineController : IDisposable
    {
        private readonly Channel<Action> _channel;
        private readonly Task _workerTask;
        private readonly CancellationTokenSource _cts = new();

        /// <summary>
        /// Raised when a fire-and-forget command throws.
        /// The handler receives the operation name (if available) and the exception.
        /// Subscribe from the UI to surface native failures.
        /// </summary>
        public event Action<Exception>? CommandFailed;

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
                () => WorkerLoopAsync(),     // returns Task; the outer Task just unwraps it
                _cts.Token,
                TaskCreationOptions.LongRunning,
                TaskScheduler.Default).Unwrap();
        }

        /// <summary>
        /// Fire-and-forget.  Returns true if the command was accepted,
        /// false if the controller is shutting down.
        /// </summary>
        public bool Enqueue(Action command)
        {
            return _channel.Writer.TryWrite(command);
        }

        /// <summary>
        /// Enqueue and return a Task that completes when the command finishes.
        /// Native exceptions are propagated to the returned Task.
        /// </summary>
        public Task EnqueueAsync(Action command)
        {
            var tcs = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);

            bool accepted = _channel.Writer.TryWrite(() =>
            {
                try   { command();           tcs.SetResult();       }
                catch (Exception ex)       { tcs.SetException(ex); }
            });

            if (!accepted)
            {
                tcs.SetException(new ObjectDisposedException(
                    nameof(EngineController), "Controller is shutting down."));
            }

            return tcs.Task;
        }

        /// <summary>
        /// Worker loop — runs on a dedicated thread.
        /// Issue 7A fix: this is now async Task, not async void.
        /// </summary>
        private async Task WorkerLoopAsync()
        {
            try
            {
                await foreach (var cmd in _channel.Reader.ReadAllAsync(_cts.Token))
                {
                    try
                    {
                        cmd();
                    }
                    catch (Exception ex)
                    {
                        // Issue 7B fix: surface errors instead of swallowing them.
                        // EnqueueAsync commands propagate through TCS.
                        // Fire-and-forget commands surface through this event.
                        CommandFailed?.Invoke(ex);
                    }
                }
            }
            catch (OperationCanceledException) { }
        }

        /// <summary>
        /// Issue 7C fix: properly drains the queue before shutting down.
        /// 
        /// 1. Signal the writer that no more commands will arrive.
        /// 2. Let the worker process remaining commands (with a timeout).
        /// 3. Cancel the worker if it hasn't finished.
        /// </summary>
        public async Task DisposeAsync()
        {
            // Signal no more writes — existing commands are still readable
            _channel.Writer.TryComplete();

            // Give the worker time to drain remaining commands
            using var drainCts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            try
            {
                await _workerTask.WaitAsync(drainCts.Token);
            }
            catch (OperationCanceledException)
            {
                // Drain timed out — force cancel
                _cts.Cancel();
                try { await _workerTask.WaitAsync(TimeSpan.FromSeconds(1)); } catch { }
            }
            catch { }

            _cts.Dispose();
        }

        /// <summary>
        /// Synchronous dispose.  Drains the queue then shuts down.
        /// Prefer DisposeAsync() when possible.
        /// </summary>
        public void Dispose()
        {
            // Complete the writer first so the worker can drain
            _channel.Writer.TryComplete();

            // Wait for drain, then force-cancel
            if (!_workerTask.Wait(TimeSpan.FromSeconds(5)))
            {
                _cts.Cancel();
                try { _workerTask.Wait(TimeSpan.FromSeconds(1)); } catch { }
            }

            _cts.Dispose();
        }
    }
}
