// Export Ghidra pseudo-C for every function, on several threads, for MH4UltimateDC agents.
//
// The same decomp/<addr>.c files as ExportForAgents' "all" mode, but each thread has its own
// decompiler, and functions whose file already exists are skipped (so an interrupted export
// resumes). Each file is written to <addr>.c.tmp and then renamed, so a stopped run never leaves
// a half-written <addr>.c; leftover .tmp files are deleted on the next run. A file that holds
// this script's "decompile failed" comment counts as not done, so failures and timeouts are
// retried. An error on one function is logged and its thread goes on; if any function is left
// without a file (and the run was not cancelled), the script ends with an error.
// Run it on a program that is already analysed and saved:
//   analyzeHeadless <proj_dir> <proj_name> -process <program> -noanalysis -readOnly \
//     -scriptPath <dir> -postScript ExportPseudoCParallel.java <out_dir> [threads]
// Keep <out_dir> OUTSIDE any git repo: it is derived from game code.
//
//@category MH4UltimateDC
//@author MH4UltimateDC contributors

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicInteger;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.util.task.TaskMonitor;

public class ExportPseudoCParallel extends GhidraScript {
    private static final int DECOMPILE_TIMEOUT_SECONDS = 60;
    // Start of the file written for a function that did not decompile (error or timeout).
    private static final String FAILED_PREFIX = "/* decompile failed: ";

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ExportPseudoCParallel.java <out_dir> [threads]");
        }
        File dir = new File(args[0], "decomp");
        dir.mkdirs();
        int threads = args.length > 1 ? Integer.parseInt(args[1]) : Math.max(1, Runtime.getRuntime().availableProcessors() - 2);

        // Leftovers of a run stopped between writing a .tmp file and renaming it.
        File[] leftovers = dir.listFiles((d, n) -> n.endsWith(".tmp"));
        if (leftovers != null) {
            for (File f : leftovers) {
                Files.delete(f.toPath());
            }
        }

        List<Function> targets = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.isThunk() && !f.isExternal() && !isDone(new File(dir, fileName(f)))) {
                targets.add(f);
            }
        }
        println(String.format("%d functions to decompile on %d threads", targets.size(), threads));

        AtomicInteger next = new AtomicInteger(), done = new AtomicInteger();
        AtomicInteger ok = new AtomicInteger(), failed = new AtomicInteger();
        ExecutorService pool = Executors.newFixedThreadPool(threads);
        int threadErrors = 0;
        try {
            List<Future<?>> workers = new ArrayList<>();
            for (int t = 0; t < threads; t++) {
                workers.add(pool.submit(() -> decompileShare(dir, targets, next, done, ok, failed)));
            }
            for (Future<?> w : workers) {
                try {
                    w.get();
                } catch (ExecutionException e) {
                    threadErrors++;
                    printerr("decompile thread: " + e.getCause());
                }
            }
        } finally {
            pool.shutdownNow();
        }

        if (monitor.isCancelled()) {
            println(String.format("cancelled after %d of %d; run again to resume", done.get(), targets.size()));
            return;
        }
        println(String.format("decompiled %d of %d (%d failed: written as failure comments, retried on the next run)",
            ok.get(), targets.size(), failed.get()));
        if (done.get() < targets.size()) {
            String msg = String.format("%d of %d functions got no file (%d decompile threads stopped with an error);"
                + " see the errors above, then run again to resume", targets.size() - done.get(), targets.size(),
                threadErrors);
            printerr(msg);
            throw new IllegalStateException(msg);
        }
    }

    // One thread: takes the next function from targets until none is left or the run is cancelled.
    // An error on one function is logged and the thread goes on; that function gets no file.
    private void decompileShare(File dir, List<Function> targets, AtomicInteger next, AtomicInteger done,
                                AtomicInteger ok, AtomicInteger failed) {
        DecompInterface di = new DecompInterface();
        try {
            // The same settings as ExportForAgents.java.
            di.toggleCCode(true);
            di.toggleSyntaxTree(false);
            di.setSimplificationStyle("decompile");
            if (!di.openProgram(currentProgram)) {
                throw new IllegalStateException("decompiler failed to start: " + di.getLastMessage());
            }
            for (int i = next.getAndIncrement(); i < targets.size() && !monitor.isCancelled();
                 i = next.getAndIncrement()) {
                Function f = targets.get(i);
                String name = fileName(f);
                File tmp = new File(dir, name + ".tmp");
                try {
                    // TaskMonitor.DUMMY: a decompile that is already running when the run is cancelled
                    // cannot be aborted (a known limit); it ends within DECOMPILE_TIMEOUT_SECONDS and
                    // the thread then stops before the next function.
                    DecompileResults r = di.decompileFunction(f, DECOMPILE_TIMEOUT_SECONDS, TaskMonitor.DUMMY);
                    boolean completed = r != null && r.decompileCompleted() && r.getDecompiledFunction() != null;
                    String body = completed
                        ? r.getDecompiledFunction().getC()
                        : FAILED_PREFIX + (r == null ? "no result" : r.getErrorMessage()) + " */\n";
                    Files.write(tmp.toPath(), body.getBytes(StandardCharsets.UTF_8));
                    Files.move(tmp.toPath(), new File(dir, name).toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                    (completed ? ok : failed).incrementAndGet();
                    int n = done.incrementAndGet();
                    if (n % 1000 == 0) {
                        println(String.format("%d / %d", n, targets.size()));
                    }
                } catch (Exception e) {
                    printerr("decompile " + name + ": " + e);
                    try {
                        Files.deleteIfExists(tmp.toPath());
                    } catch (IOException ignored) {
                        // Deleted on the next run.
                    }
                }
            }
        } finally {
            di.dispose();
        }
    }

    private static String fileName(Function f) {
        return String.format("%08x.c", f.getEntryPoint().getOffset());
    }

    // True if out exists and is not a failure comment written by this script.
    private static boolean isDone(File out) throws IOException {
        if (!out.isFile()) {
            return false;
        }
        byte[] marker = FAILED_PREFIX.getBytes(StandardCharsets.UTF_8);
        byte[] head = new byte[marker.length];
        int n;
        try (InputStream in = Files.newInputStream(out.toPath())) {
            n = in.readNBytes(head, 0, head.length);
        }
        return !(n == marker.length && Arrays.equals(head, marker));
    }
}
