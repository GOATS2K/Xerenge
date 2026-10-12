package com.xerenge.burnout;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The logs, copied into a folder the player picked (the installer's "Log
 * folder"). The game writes burnout.log into the app's own external files
 * directory, which a phone's file manager cannot open any more (Android 11 on),
 * so a player reporting a problem had no way to get at it. A picked folder is a
 * document tree, not a path the game could write to, so the files are copied:
 * when the game starts (the last run's logs - the game appends, so a run that
 * crashed or hung is still there) and whenever it goes to the background.
 */
final class LogExport {
    static final String PREF_TREE = "log_tree";  // the picked folder's tree URI, or absent

    private static final String[] NAMES = {"burnout.log", "burnout.1.log", "burnout.2.log", "burnout.3.log"};
    private static final Object running = new Object();
    private static boolean busy;

    private LogExport() {
    }

    static Uri tree(Context context) {
        String value = context.getSharedPreferences(InstallerActivity.PREFS, Context.MODE_PRIVATE)
                .getString(PREF_TREE, null);
        return value == null ? null : Uri.parse(value);
    }

    /** Copies the logs on a thread of its own; one copy at a time, a request during one is dropped. */
    static void exportAsync(Context context) {
        Uri tree = tree(context);
        if (tree == null) {
            return;
        }
        synchronized (running) {
            if (busy) {
                return;
            }
            busy = true;
        }
        Context app = context.getApplicationContext();
        Thread thread = new Thread(() -> {
            try {
                export(app, tree);
            } catch (Exception error) {
                Log.w("xerenge", "copying the logs failed", error);
            } finally {
                synchronized (running) {
                    busy = false;
                }
            }
        }, "log export");
        thread.setPriority(Thread.MIN_PRIORITY);
        thread.start();
    }

    private static void export(Context context, Uri tree) throws Exception {
        ContentResolver resolver = context.getContentResolver();
        Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        File root = Settings.root(context);
        byte[] buffer = new byte[1 << 16];
        for (String name : NAMES) {
            File source = new File(root, name);
            if (!source.isFile()) {
                continue;
            }
            Uri target = find(resolver, tree, name);
            if (target == null) {
                // octet-stream: with text/plain some providers add ".txt" to the name.
                target = DocumentsContract.createDocument(resolver, parent, "application/octet-stream", name);
            }
            if (target == null) {
                continue;
            }
            try (InputStream in = new FileInputStream(source);
                 OutputStream out = resolver.openOutputStream(target, "wt")) {
                if (out == null) {
                    continue;
                }
                int got;
                while ((got = in.read(buffer)) > 0) {
                    out.write(buffer, 0, got);
                }
            }
        }
    }

    private static Uri find(ContentResolver resolver, Uri tree, String name) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor cursor = resolver.query(children, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME},
                null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                if (name.equals(cursor.getString(1))) {
                    return DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0));
                }
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    /** What the installer shows for the picked folder. */
    static String folderName(Context context) {
        Uri tree = tree(context);
        if (tree == null) {
            return null;
        }
        String id = DocumentsContract.getTreeDocumentId(tree);
        // "primary:Download/Burnout" -> "Download/Burnout"
        int colon = id.indexOf(':');
        return colon >= 0 && colon + 1 < id.length() ? id.substring(colon + 1) : id;
    }
}
