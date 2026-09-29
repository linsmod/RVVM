package com.rvvm.android;

import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;
import java.nio.charset.StandardCharsets;

/**
 * A scripted view of the guest console.
 *
 * <p>The console is otherwise reachable two ways only: a person taps the screen
 * and types, or an agent takes a screenshot and replays taps. Both are the same
 * interface wearing a costume - neither one can wait for a prompt, see a byte
 * the guest wrote, or tell "nothing yet" from "not looking". This server is the
 * third door: a plain text protocol over TCP, reachable with
 * {@code adb forward tcp:7979 tcp:7979}, so a driver on the other end can read
 * the screen as text and type into it.</p>
 *
 * <p>It is deliberately pull-only. The console is screen state, not a byte
 * stream, so the honest question is "what does the screen say now" and the
 * honest answer is one consistent snapshot - no partial frame, no ordering
 * question between a push and a pull, and nothing to buffer. A client that
 * wants to be told when something changed polls {@code serial} and only pays
 * for a full read when it moved.</p>
 *
 * <p>Bound to loopback only. A debug channel reachable from the network is a
 * remote shell into every guest on the device, so the listener is opened on
 * 127.0.0.1 and reached through {@code adb forward} rather than through the
 * device's own interfaces.</p>
 *
 * <h3>Protocol</h3>
 * A request is one ASCII line, newline-terminated. A response is a status
 * line, then zero or more payload lines, then a line holding a single dot -
 * so a client always knows where the payload ends without counting lines:
 *
 * <pre>
 *   + key=value key=value\n     ok, followed by the fields
 *   - reason\n                 refused, no payload
 *   ...payload lines...\n
 *   .\n                        end of response
 * </pre>
 *
 * Requests:
 * <ul>
 *   <li>{@code attach [guestId]} - select a console (default: the foreground
 *       one), pin its view to the live screen, and return it right away so a
 *       client's first read costs one round trip</li>
 *   <li>{@code snap} - the screen: header line plus one line per row</li>
 *   <li>{@code follow} - pin the view home (a user scrolled back in the UI
 *       otherwise decides what a script sees)</li>
 *   <li>{@code in <hex>} - type bytes into the console. Hex, so the bytes a
 *       terminal protocol actually needs survive: {@code 0d} is Enter,
 *       {@code 03} is Ctrl-C, {@code 1b5b41} is the up arrow. The core's line
 *       discipline runs them (ICRNL, ISIG, erase, echo), so a client types
 *       keystrokes and not terminal escapes.</li>
 *   <li>{@code resize <rows>} - grid height; the guest sees it through
 *       TIOCGWINSZ</li>
 *   <li>{@code scroll <lines>} - drag the view through the scrollback</li>
 *   <li>{@code status} - what the attached guest is doing</li>
 *   <li>{@code list} - the runs in the table</li>
 *   <li>{@code ping} / {@code exit}</li>
 * </ul>
 *
 * <p>The guest's own stdio is line buffered, so output that a guest never
 * flushed stays in its buffer no matter who is reading - that is the guest's
 * buffering, not this channel's, and a guest that wants its progress lines
 * live has to fflush.</p>
 */
final class RvvmConsoleServer {

    private static final String TAG = "RVVM-Console";

    /** The system property that turns the server on. Its value is the port. */
    static final String PROP = "debug.rvvm.console";

    /** Loopback, never INADDR_ANY: see the class comment. */
    private static final String BIND_ADDRESS = "127.0.0.1";
    static final int DEFAULT_PORT = 7979;

    /**
     * Longest request line accepted. A client that sends more is a client that
     * is broken or hostile, and readLine() would happily buffer all of it.
     */
    private static final int MAX_REQUEST_BYTES = 64 * 1024;

    private ServerSocket listener;
    private Thread acceptThread;
    private volatile boolean stopping;

    /** Port actually bound, 0 when the server is not running. */
    private volatile int port;

    synchronized void start(int requestedPort) {
        if (listener != null) {
            return;
        }
        try {
            ServerSocket ss = new ServerSocket();
            ss.setReuseAddress(true);
            ss.bind(new java.net.InetSocketAddress(InetAddress.getByName(BIND_ADDRESS),
                    requestedPort), 4);
            listener = ss;
            port = ss.getLocalPort();
        } catch (IOException e) {
            listener = null;
            port = 0;
            Log.w(TAG, "console server not started on port " + requestedPort + ": " + e);
            return;
        }
        stopping = false;
        acceptThread = new Thread(this::acceptLoop, "rvvm-console-accept");
        acceptThread.setDaemon(true);
        acceptThread.start();
        Log.i(TAG, "console server listening on " + BIND_ADDRESS + ":" + port
                + "  (adb forward tcp:" + port + " tcp:" + port + ")");
    }

    synchronized void stop() {
        stopping = true;
        ServerSocket ss = listener;
        listener = null;
        port = 0;
        if (ss != null) {
            try {
                ss.close();   // unblocks the accept
            } catch (IOException ignored) {
                // closing a listener that is already gone is not a failure
            }
        }
    }

    synchronized boolean isRunning() {
        return listener != null;
    }

    int port() {
        return port;
    }

    private void acceptLoop() {
        while (!stopping) {
            ServerSocket ss = listener;
            if (ss == null) {
                return;
            }
            Socket client;
            try {
                client = ss.accept();
            } catch (IOException e) {
                return;   // stop() closed it, or the listener died
            }
            Thread t = new Thread(() -> serve(client), "rvvm-console-client");
            t.setDaemon(true);
            t.start();
        }
    }

    // ---- one connection -------------------------------------------------

    /** One attached console. -1 means "whichever is in the foreground". */
    private int guestId = -1;

    private void serve(Socket socket) {
        try {
            socket.setTcpNoDelay(true);
            socket.setSoTimeout(0);
            InputStream in = socket.getInputStream();
            OutputStream out = socket.getOutputStream();

            while (!socket.isClosed()) {
                String line = readLine(in);
                if (line == null) {
                    return;                       // client hung up
                }
                if (line.isEmpty()) {
                    continue;
                }
                if (!dispatch(line, out)) {
                    return;                       // the client asked to leave
                }
            }
        } catch (SocketException e) {
            // the peer went away mid-exchange; nothing left to report to
        } catch (IOException e) {
            Log.w(TAG, "console client: " + e);
        } catch (Throwable t) {
            // Deliberately past Error, not just Exception. An uncaught throwable
            // on this thread takes the whole process with it - and this process
            // is the one running the guest. A debug channel that can kill the
            // thing it exists to observe is worse than no channel, so the worst
            // a misbehaving client gets is a dropped connection and a log line.
            Log.e(TAG, "console client failed: " + t, t);
        } finally {
            try {
                socket.close();
            } catch (IOException ignored) {
                // already gone
            }
        }
    }

    /**
     * Handle one request. Returns false when the connection should close.
     */
    private boolean dispatch(String line, OutputStream out) throws IOException {
        String verb;
        String arg;
        int sp = line.indexOf(' ');
        if (sp < 0) {
            verb = line;
            arg = null;
        } else {
            verb = line.substring(0, sp);
            arg = line.substring(sp + 1).trim();
        }

        switch (verb) {
            case "attach": {
                int id = -1;
                if (arg != null && !arg.isEmpty()) {
                    try {
                        id = Integer.parseInt(arg);
                    } catch (NumberFormatException e) {
                        refuse(out, "attach takes a guest id, got '" + arg + "'");
                        return true;
                    }
                }
                guestId = id;
                // Home first: an attach that answered from a view the user had
                // scrolled back would be answering a different question than
                // the client asked.
                RvvmNative.nativeTtyFollow(guestId);
                screen(out);
                return true;
            }
            case "snap":
                screen(out);
                return true;
            case "follow":
                RvvmNative.nativeTtyFollow(guestId);
                ok(out, "");
                return true;
            case "in":
                input(out, arg);
                return true;
            case "resize": {
                int rows = parseInt(arg, -1);
                if (rows < 1) {
                    refuse(out, "resize takes a row count");
                    return true;
                }
                RvvmNative.nativeTtyResize(rows, 80);
                ok(out, "rows=" + rows);
                return true;
            }
            case "scroll": {
                int lines = parseInt(arg, 0);
                RvvmNative.nativeTtyScrollBy(guestId, lines);
                ok(out, "lines=" + lines);
                return true;
            }
            case "status": {
                ok(out, "guest=" + guestId
                        + " running=" + (RvvmNative.nativeIsGuestRunning(guestId) ? 1 : 0)
                        + " parked=" + (RvvmNative.nativeIsGuestParked(guestId) ? 1 : 0)
                        + " screen=" + (RvvmNative.nativeTtyScreen(guestId) != null ? 1 : 0));
                return true;
            }
            case "ping":
                ok(out, "pong");
                return true;
            case "exit":
                ok(out, "bye");
                return false;
            default:
                refuse(out, "unknown command '" + verb + "'");
                return true;
        }
    }

    /**
     * The attached console as text. A guest with no session yet - or an id that
     * was never in the run table - answers with the fact rather than with
     * somebody else's screen: nativeTtyScreen() resolves an explicit id to that
     * run only, and null when there is none.
     */
    private void screen(OutputStream out) throws IOException {
        String text = RvvmNative.nativeTtyScreen(guestId);
        if (text == null) {
            ok(out, "guest=" + guestId + " screen=none running="
                    + (RvvmNative.nativeIsGuestRunning(guestId) ? 1 : 0));
            return;
        }
        int nl = text.indexOf('\n');
        if (nl < 0) {
            ok(out, text);
            return;
        }
        // The header opens the response and the rows are its payload, so the
        // terminator is written once, after both - not by the header helper.
        header(out, text.substring(0, nl));
        StringBuilder sb = new StringBuilder();
        for (String row : text.substring(nl + 1).split("\n", -1)) {
            sb.append(escapeRow(row)).append('\n');
        }
        out.write(sb.toString().getBytes(StandardCharsets.UTF_8));
        end(out);
    }

    /**
     * Make a payload row safe to send, reversibly.
     *
     * <p>A dot-terminated response cannot carry a row that is itself a dot, and
     * a bare dot is a real thing on a terminal - the entry in a directory
     * listing, the marker a progress bar prints. Padding it to " ." would be
     * the obvious fix and the wrong one: the client could no longer tell a row
     * that said "." from one that said " .", and this is a channel whose whole
     * job is reporting what the screen said.</p>
     *
     * <p>So a leading backslash is the escape, doubling itself and standing in
     * for a leading dot. A row that starts with neither is untouched, which is
     * every row a terminal actually draws.</p>
     */
    static String escapeRow(String row) {
        if (row.equals(".")) {
            return "\\.";
        }
        return row.startsWith("\\") ? "\\" + row : row;
    }

    /**
     * Type into the attached console.
     *
     * <p>Refused when the run is not running, rather than left to native:
     * nativeTtyInput resolves an id it does not recognise by falling back to the
     * active run, which for a server would mean silently typing into a
     * different guest than the client named. A refusal does not close the
     * connection - the client asked a wrong question, not for the end of
     * everything.</p>
     */
    private void input(OutputStream out, String hex) throws IOException {
        byte[] bytes;
        try {
            bytes = unhex(hex);
        } catch (IllegalArgumentException e) {
            refuse(out, String.valueOf(e.getMessage()));
            return;
        }
        if (bytes.length == 0) {
            refuse(out, "in takes hex bytes, e.g. 'in 6c730d' for `ls` + Enter");
            return;
        }
        if (!RvvmNative.nativeIsGuestRunning(guestId)) {
            refuse(out, "guest " + guestId + " is not running; nothing to type into");
            return;
        }
        RvvmNative.nativeTtyInput(guestId, bytes);
        ok(out, "n=" + bytes.length);
    }

    private static int parseInt(String s, int fallback) {
        if (s == null || s.isEmpty()) {
            return fallback;
        }
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    static byte[] unhex(String hex) {
        if (hex == null) {
            return new byte[0];
        }
        String s = hex.trim();
        if ((s.length() & 1) != 0) {
            throw new IllegalArgumentException("hex must have an even digit count");
        }
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            int hi = Character.digit(s.charAt(i * 2), 16);
            int lo = Character.digit(s.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) {
                throw new IllegalArgumentException("'" + s.charAt(i * 2)
                        + s.charAt(i * 2 + 1) + "' is not a hex byte");
            }
            out[i] = (byte)((hi << 4) | lo);
        }
        return out;
    }

    // ---- framing --------------------------------------------------------

    /** The status line, without the terminator: a response that carries a payload
 *  writes its header, then the payload, then ends once. */
    private static void header(OutputStream out, String text) throws IOException {
        out.write(("+ " + text + "\n").getBytes(StandardCharsets.UTF_8));
    }

    /** A status line and the end of a response with no payload. */
    private static void ok(OutputStream out, String text) throws IOException {
        header(out, text);
        end(out);
    }

    /**
     * A refusal. The connection stays up: the client asked a wrong question,
     * not for the end of everything, and a driver that mistypes one hex digit
     * should not have to reconnect.
     */
    private static void refuse(OutputStream out, String reason) throws IOException {
        out.write(("- " + reason + "\n").getBytes(StandardCharsets.UTF_8));
        end(out);
    }

    private static void end(OutputStream out) throws IOException {
        out.write(".\n".getBytes(StandardCharsets.UTF_8));
        out.flush();
    }

    /**
     * One newline-terminated line, or null at end of stream. Bounded: a client
     * that never sends a newline must not be able to grow this without limit.
     *
     * <p>The buffer is turned into a String by hand rather than through
     * {@code ByteArrayOutputStream.toString(Charset)}, which is Java 10 and
     * absent from the libcore this app actually runs on: the Gradle config
     * compiles against JDK 11, so javac accepts that call happily and the
     * device only finds out when a client connects.</p>
     */
    private static String readLine(InputStream in) throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream(256);
        int c;
        while ((c = in.read()) >= 0) {
            if (c == '\n') {
                return new String(buf.toByteArray(), StandardCharsets.UTF_8);
            }
            if (c != '\r') {
                if (buf.size() >= MAX_REQUEST_BYTES) {
                    throw new IOException("request line over " + MAX_REQUEST_BYTES + " bytes");
                }
                buf.write(c);
            }
        }
        return buf.size() > 0 ? new String(buf.toByteArray(), StandardCharsets.UTF_8) : null;
    }
}
