package com.re5.recompiled;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.util.Log;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Locale;
import java.util.UUID;

/**
 * Enlace Bluetooth (RFCOMM) ponto a ponto do multiplayer LAN. O runtime em C (bt_link.c) chama pela
 * GameActivity: open() conecta ou aceita (bloqueia ate ~2 s e e repetido de fora), read() devolve o proximo
 * quadro (u16 de tamanho + dados) ou null se o enlace caiu, write() envia um quadro.
 *
 * cfg: "host" = espera o outro aparelho; qualquer outra coisa = nome (ou parte) ou endereco de um aparelho
 * PAREADO. O UUID e o mesmo do servico que o re5.exe registra no Windows (bt_link.c).
 */
final class BtLink {
    private static final String TAG = "RE5Bt";
    static final UUID SERVICE = UUID.fromString("5e5b7c1a-52e5-4c4f-9a3b-1d5e0a5e5001");
    private static final int MAX_FRAME = 1500;

    private BluetoothServerSocket server;   // fica aberto entre tentativas: o registro do servico (SDP) continua
    private volatile BluetoothSocket sock;
    private volatile DataInputStream in;
    private volatile OutputStream out;
    private final Object writeLock = new Object();

    @SuppressLint("MissingPermission")
    boolean open(Context ctx, String cfg) {
        try {
            BluetoothManager bm = (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = bm != null ? bm.getAdapter() : null;
            if (ad == null || !ad.isEnabled()) {
                Log.w(TAG, "Bluetooth desligado ou ausente");
                return false;
            }
            BluetoothSocket s;
            if ("host".equalsIgnoreCase(cfg)) {
                if (server == null) {
                    server = ad.listenUsingRfcommWithServiceRecord("RE5 Co-op", SERVICE);
                    Log.i(TAG, "aguardando o outro aparelho (RFCOMM)");
                }
                s = server.accept(2000);   // IOException no prazo: tenta de novo la fora
            } else {
                BluetoothDevice dev = find(ad, cfg);
                if (dev == null) {
                    Log.w(TAG, "aparelho '" + cfg + "' nao esta entre os pareados");
                    return false;
                }
                try { ad.cancelDiscovery(); } catch (SecurityException ignored) { }
                s = dev.createRfcommSocketToServiceRecord(SERVICE);
                s.connect();
            }
            in = new DataInputStream(s.getInputStream());
            out = s.getOutputStream();
            sock = s;
            Log.i(TAG, "enlace conectado");
            return true;
        } catch (IOException | SecurityException e) {
            if (!(e instanceof IOException) || !String.valueOf(e.getMessage()).contains("timed out"))
                Log.w(TAG, "open: " + e);
            return false;
        }
    }

    @SuppressLint("MissingPermission")
    private static BluetoothDevice find(BluetoothAdapter ad, String want) {
        String w = want.toLowerCase(Locale.ROOT);
        for (BluetoothDevice d : ad.getBondedDevices()) {
            String n = d.getName();
            if (d.getAddress().equalsIgnoreCase(want) || (n != null && n.toLowerCase(Locale.ROOT).contains(w))) return d;
        }
        return null;
    }

    /** Proximo quadro; null = o enlace caiu. */
    byte[] read() {
        DataInputStream i = in;
        if (i == null) return null;
        try {
            int n = i.readUnsignedShort();
            if (n <= 0 || n > MAX_FRAME) return null;
            byte[] b = new byte[n];
            i.readFully(b);
            return b;
        } catch (IOException e) {
            return null;
        }
    }

    boolean write(byte[] d) {
        OutputStream o = out;
        if (o == null || d == null || d.length == 0 || d.length > MAX_FRAME) return false;
        byte[] f = new byte[d.length + 2];
        f[0] = (byte) (d.length >> 8);
        f[1] = (byte) d.length;
        System.arraycopy(d, 0, f, 2, d.length);
        try {
            synchronized (writeLock) {
                o.write(f);
                o.flush();
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    void close() {
        BluetoothSocket s = sock;
        sock = null;
        in = null;
        out = null;
        if (s != null) {
            try { s.close(); } catch (IOException ignored) { }
        }
    }
}
