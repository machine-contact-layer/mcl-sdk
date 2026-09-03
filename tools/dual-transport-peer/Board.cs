// The board's serial console: the run's second, independent record.
//
// WHY THE HARNESS OWNS THE SERIAL PORT
//
// The board's own counters are the ground truth for what it received. A host
// that reports a failure while the board reports a clean refusal is describing
// a lost frame, not a protocol defect, and the two records only settle that
// question when they are read together. Capturing the log in a separate process
// would work, except that this experiment also has to ARM the board's
// deliberate-drop command at an exact point in the sequence, and two processes
// cannot both hold COM3.
//
// So the harness holds it, writes every line to the evidence log, and sends the
// three commands the sequence needs.

using System;
using System.Collections.Concurrent;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Threading;

namespace Mcl.Sdk.DualTransport
{
    internal sealed class Board : IDisposable
    {
        private readonly SerialPort _port;
        private readonly StreamWriter _log;
        private readonly BlockingCollection<string> _lines = new();
        private readonly StringBuilder _partial = new();

        private readonly Thread _reader;
        private volatile bool _stop;

        public long BytesRead { get; private set; }

        public Board(string portName, string logPath)
        {
            _log = new StreamWriter(logPath, append: false) { AutoFlush = true };

            // The ESP32-S3 speaks USB CDC, which ignores the line settings; they
            // are supplied because the API requires them.
            _port = new SerialPort(portName, 115200, Parity.None, 8, StopBits.One)
            {
                ReadTimeout = 250,
                WriteTimeout = 2000,
                NewLine = "\n",
                DtrEnable = true,
                ReadBufferSize = 65536
            };
            _port.Open();

            /*
             * A DEDICATED BLOCKING READER, NOT THE DataReceived EVENT.
             *
             * The first version of this used SerialPort.DataReceived. It worked
             * for a short run and then stopped firing partway through a hundred
             * migrations: the board's log went silent from cycle 40 onward and it
             * stopped answering STATUS altogether. Nothing was wrong with the
             * board. Its logging is guarded by Serial.availableForWrite() and
             * drops a line rather than blocking, so a host that stops draining
             * the port silently switches the board's entire evidence record off.
             *
             * An instrument that stops recording without saying so is worse than
             * no instrument, and it took a run to notice. A thread that only
             * reads cannot stop being scheduled.
             */
            _reader = new Thread(ReadLoop) { IsBackground = true, Name = "mcl-board-serial" };
            _reader.Start();
        }

        private void ReadLoop()
        {
            var buffer = new byte[4096];
            var stream = _port.BaseStream;

            while (!_stop)
            {
                int n;
                try { n = stream.Read(buffer, 0, buffer.Length); }
                catch (TimeoutException) { continue; }
                catch (Exception ex)
                {
                    if (_stop) { return; }
                    _log.WriteLine($"[harness] serial read failed: {ex.Message}");
                    return;
                }
                if (n <= 0) { continue; }

                BytesRead += n;
                Consume(Encoding.ASCII.GetString(buffer, 0, n));
            }
        }

        private void Consume(string chunk)
        {
            foreach (char c in chunk)
            {
                if (c == '\n')
                {
                    string line = _partial.ToString().TrimEnd('\r');
                    _partial.Clear();
                    if (line.Length > 0)
                    {
                        _log.WriteLine(line);
                        _lines.Add(line);
                    }
                }
                else { _partial.Append(c); }
            }
        }

        /*
         * Drain before commanding. Everything already queued has been written to
         * the log, which is the evidence; leaving it in the queue only means the
         * answer to this command has to be found behind it.
         */
        public void Send(string command)
        {
            while (_lines.TryTake(out _)) { }
            _log.WriteLine($"[harness] -> {command}");
            _port.Write(command + "\n");
        }

        /*
         * Wait for a line starting with `prefix`, discarding the asynchronous
         * MCLDUAL event lines that arrive in between. Those are not noise -- they
         * are the evidence record -- so they are still written to the log; they
         * are simply not the answer to this question.
         */
        public string Await(string prefix, int timeoutMs)
        {
            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline)
            {
                int remaining = (int)(deadline - DateTime.UtcNow).TotalMilliseconds;
                if (remaining <= 0) { break; }
                if (!_lines.TryTake(out string line, remaining)) { continue; }
                if (line.StartsWith(prefix, StringComparison.Ordinal)) { return line; }
            }
            return null;
        }

        /* One STATUS line, parsed into its key=value pairs. */
        public System.Collections.Generic.Dictionary<string, string> Status(int timeoutMs = 4000)
        {
            Send("STATUS");
            string line = Await("MCLDUAL STATUS", timeoutMs);
            var fields = new System.Collections.Generic.Dictionary<string, string>(StringComparer.Ordinal);
            if (line == null) { return fields; }

            foreach (string token in line.Split(' ', StringSplitOptions.RemoveEmptyEntries))
            {
                int eq = token.IndexOf('=');
                if (eq > 0) { fields[token.Substring(0, eq)] = token.Substring(eq + 1); }
            }
            return fields;
        }

        public static long Counter(System.Collections.Generic.Dictionary<string, string> status, string key)
        {
            if (status != null && status.TryGetValue(key, out string value) &&
                long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out long n))
            {
                return n;
            }
            return -1;
        }

        public void Note(string text) => _log.WriteLine($"[harness] {text}");

        public void Dispose()
        {
            _stop = true;
            try { _port.Close(); } catch { /* closing */ }
            _reader?.Join(1500);
            _port.Dispose();
            _log.Dispose();
        }
    }
}
