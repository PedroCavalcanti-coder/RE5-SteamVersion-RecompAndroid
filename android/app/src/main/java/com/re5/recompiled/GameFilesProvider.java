package com.re5.recompiled;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * Mostra a pasta do jogo no app Arquivos do Android (e em qualquer seletor de
 * arquivos) como "Resident Evil 5". Sem isso a pasta Android/data do app fica
 * invisível para o usuário a partir do Android 11.
 *
 * O id de documento é o caminho relativo à pasta do jogo ("" é a raiz).
 */
public final class GameFilesProvider extends DocumentsProvider {
    public static final String AUTHORITY = "com.re5.recompiled.files";
    public static final String ROOT_ID = "re5";
    public static final String ROOT_DOC_ID = "";

    private static final String[] ROOT_COLUMNS = {
            Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_SUMMARY,
            Root.COLUMN_DOCUMENT_ID, Root.COLUMN_ICON, Root.COLUMN_AVAILABLE_BYTES,
    };
    private static final String[] DOC_COLUMNS = {
            Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
            Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS,
    };

    private File base;

    @Override
    public boolean onCreate() {
        return true;
    }

    private File base() {
        if (base == null) base = GameStorage.gameDir(getContext());
        return base;
    }

    private File fileFor(String docId) throws FileNotFoundException {
        File f = docId.isEmpty() ? base() : new File(base(), docId);
        try {
            String root = base().getCanonicalPath();
            String path = f.getCanonicalPath();
            if (!path.equals(root) && !path.startsWith(root + File.separator)) {
                throw new FileNotFoundException(docId);
            }
        } catch (IOException e) {
            throw new FileNotFoundException(docId);
        }
        if (!f.exists()) throw new FileNotFoundException(docId);
        return f;
    }

    private String docIdFor(File f) {
        String root = base().getAbsolutePath();
        String path = f.getAbsolutePath();
        return path.equals(root) ? ROOT_DOC_ID : path.substring(root.length() + 1);
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        MatrixCursor.RowBuilder row = c.newRow();
        row.add(Root.COLUMN_ROOT_ID, ROOT_ID);
        row.add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_IS_CHILD);
        row.add(Root.COLUMN_TITLE, "Resident Evil 5");
        row.add(Root.COLUMN_SUMMARY, "Arquivos do jogo");
        row.add(Root.COLUMN_DOCUMENT_ID, ROOT_DOC_ID);
        row.add(Root.COLUMN_ICON, R.mipmap.ic_launcher);
        row.add(Root.COLUMN_AVAILABLE_BYTES, base().getUsableSpace());
        return c;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        addRow(c, fileFor(documentId));
        return c;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        File[] children = fileFor(parentDocumentId).listFiles();
        if (children != null) {
            for (File f : children) addRow(c, f);
        }
        return c;
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        return parentDocumentId.isEmpty() || documentId.startsWith(parentDocumentId + "/");
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileFor(documentId), ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public void deleteDocument(String documentId) throws FileNotFoundException {
        if (documentId.isEmpty()) throw new FileNotFoundException("a raiz não pode ser apagada");
        if (!GameStorage.deleteTree(fileFor(documentId))) {
            throw new IllegalStateException("não foi possível apagar " + documentId);
        }
    }

    @Override
    public String getDocumentType(String documentId) throws FileNotFoundException {
        return mimeOf(fileFor(documentId));
    }

    private void addRow(MatrixCursor c, File f) {
        int flags = Document.FLAG_SUPPORTS_DELETE;
        if (f.isDirectory()) flags |= Document.FLAG_DIR_PREFERS_LAST_MODIFIED;
        else flags |= Document.FLAG_SUPPORTS_WRITE;
        boolean isRoot = f.getAbsolutePath().equals(base().getAbsolutePath());
        if (isRoot) flags &= ~Document.FLAG_SUPPORTS_DELETE;

        MatrixCursor.RowBuilder row = c.newRow();
        row.add(Document.COLUMN_DOCUMENT_ID, docIdFor(f));
        row.add(Document.COLUMN_DISPLAY_NAME, isRoot ? "Resident Evil 5" : f.getName());
        row.add(Document.COLUMN_MIME_TYPE, mimeOf(f));
        row.add(Document.COLUMN_SIZE, f.isDirectory() ? null : f.length());
        row.add(Document.COLUMN_LAST_MODIFIED, f.lastModified());
        row.add(Document.COLUMN_FLAGS, flags);
    }

    private static String mimeOf(File f) {
        if (f.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = f.getName();
        int dot = name.lastIndexOf('.');
        if (dot >= 0) {
            String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase());
            if (mime != null) return mime;
        }
        return "application/octet-stream";
    }
}
