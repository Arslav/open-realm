package org.openrealm.wc3;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/** Per-file transaction: incomplete or invalid copies never replace a working archive. */
public final class ArchiveStore {
    private static final List<String> ARCHIVES = Arrays.asList("war3.mpq", "war3local.mpq", "war3x.mpq",
        "war3xlocal.mpq", "war3patch.mpq", "war3xpatch.mpq");
    public interface Validator { boolean valid(String path); }

    public static String archiveName(String name) throws IOException {
        if (name == null) throw new IOException("Selected file has no name");
        String lower = name.toLowerCase(Locale.ROOT);
        if (!ARCHIVES.contains(lower))
            throw new IOException("Unsupported archive: " + name);
        return lower;
    }

    /** Movies are AVI media despite their historical MPQ suffix; never mount them as game archives. */
    public static String assetName(String name) throws IOException {
        if (name == null || !name.matches("[A-Za-z0-9_-]+\\.(?i:mpq|avi)"))
            throw new IOException("Unsupported Warcraft III file: " + name);
        String lower = name.toLowerCase(Locale.ROOT);
        if (ARCHIVES.contains(lower)) return lower;
        return "Movies/" + name;
    }

    private static boolean validMovie(File file) throws IOException {
        if (file.length() < 12) return false;
        byte[] header = new byte[12];
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) { input.readFully(header); }
        long size = 0;
        for (int i = 0; i < 4; i++) size |= ((long)header[4 + i] & 255) << (i * 8);
        return header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
            header[8] == 'A' && header[9] == 'V' && header[10] == 'I' && header[11] == ' ' &&
            size >= 4 && size + 8 <= file.length();
    }

    public static long copy(File directory, String name, InputStream input, long expected, Validator validator)
            throws IOException {
        File temporary = null;
        try {
            String target;
            long copied = 0;
            try (InputStream source = input) {
                target = assetName(name);
                if (source == null) throw new IOException("Cannot open " + name);
                if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create data directory");
                File parent = new File(directory, target).getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create movie directory");
                if (expected > directory.getUsableSpace()) throw new IOException("Not enough free space for " + name);
                temporary = File.createTempFile(".import-", ".tmp", parent);
                try (FileOutputStream output = new FileOutputStream(temporary)) {
                    byte[] buffer = new byte[1024 * 1024];
                    int count;
                    while ((count = source.read(buffer)) != -1) {
                        output.write(buffer, 0, count);
                        copied += count;
                    }
                    output.getFD().sync();
                }
            }
            // Provider close errors must occur before committing the replacement.
            if (copied == 0 || (expected >= 0 && copied != expected)) throw new IOException("Incomplete archive: " + name);
            if (target.startsWith("Movies/") ? !validMovie(temporary) : !validator.valid(temporary.getAbsolutePath()))
                throw new IOException("Invalid Warcraft III file: " + name);
            Files.move(temporary.toPath(), new File(directory, target).toPath(),
                StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            return copied;
        } finally {
            if (temporary != null) Files.deleteIfExists(temporary.toPath());
        }
    }
}
