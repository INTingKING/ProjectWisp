package org.projectwisp.app;

import android.app.DownloadManager;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;

import java.io.File;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

/** SDL activity plus a tiny DownloadManager shim, called from native code (src/util/download.cpp). */
public class WispActivity extends SDLActivity {
    private static DownloadManager dm() {
        return (DownloadManager) SDL.getContext().getSystemService(Context.DOWNLOAD_SERVICE);
    }

    /** Enqueue url -> path. Files under getExternalFilesDir(null) need no storage permission. Returns the request id. */
    public static long startDownload(String url, String path, String title) {
        Context ctx = SDL.getContext();
        File f = new File(path);
        f.delete();
        File parent = f.getParentFile();
        if (parent != null) parent.mkdirs();
        DownloadManager.Request r = new DownloadManager.Request(Uri.parse(url));
        File ext = ctx.getExternalFilesDir(null);
        String root = ext != null ? ext.getAbsolutePath() + "/" : null;
        if (root != null && path.startsWith(root)) r.setDestinationInExternalFilesDir(ctx, null, path.substring(root.length()));
        else r.setDestinationUri(Uri.fromFile(f));
        r.setTitle(title);
        r.setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE);
        r.setAllowedOverMetered(true);
        r.setAllowedOverRoaming(false);
        return dm().enqueue(r);
    }

    /** {status, bytesSoFar, totalBytes, reason}; status 0 = unknown id. */
    public static long[] queryDownload(long id) {
        long[] out = new long[] {0, 0, -1, 0};
        Cursor c = dm().query(new DownloadManager.Query().setFilterById(id));
        if (c == null) return out;
        try {
            if (c.moveToFirst()) {
                out[0] = c.getInt(c.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS));
                out[1] = c.getLong(c.getColumnIndexOrThrow(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR));
                out[2] = c.getLong(c.getColumnIndexOrThrow(DownloadManager.COLUMN_TOTAL_SIZE_BYTES));
                out[3] = c.getInt(c.getColumnIndexOrThrow(DownloadManager.COLUMN_REASON));
            }
        } finally {
            c.close();
        }
        return out;
    }

    public static void cancelDownload(long id) { dm().remove(id); }
}
