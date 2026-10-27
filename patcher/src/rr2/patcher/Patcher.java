package rr2.patcher;

import com.android.apksig.ApkSigner;

import java.io.*;
import java.security.*;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.*;
import java.util.function.Consumer;
import java.util.zip.*;

/** The same changes as tools/build.sh, made directly on the APK's zip entries. Runs on the phone and on PC. */
public final class Patcher {
    static final String GUEST = "lib/armeabi-v7a/libRecklessRacing2.so";
    static final int MIN_TARGET_SDK = 24;                // Android 14+ refuses older
    static final int ATTR_TARGET_SDK = 0x01010270;       // android:targetSdkVersion
    static final int ATTR_MIN_SDK = 0x0101020c;          // android:minSdkVersion
    static final String OBB = "rr2/main.obb";            // stored uncompressed; the loader reads it straight from the APK
    static final String[][] DEX_PATCHES = {              // keep in sync with tools/patch_dex.py
        {"13160500", "13160800"},          // FuseEgl: config chooser's red size 5 -> 8 (RGB565 -> RGB888)
        {"6200960c2104", "12150f052104"},  // FileCheck.expansionFilesDelivered: return true (OBB is in the APK)
        {"7210fa1c01000a0139010b00", "7210fa1c01000a0129000b00"},  // IabHelper.startSetup: report billing unavailable
    };

    public static void patch(File in, File obb, InputStream loader, File out, PrivateKey key, X509Certificate cert,
                             Consumer<String> log) throws Exception {
        File unsigned = new File(out.getPath() + ".unsigned"), inner = new File(out.getPath() + ".inner");
        try {
            in = unwrapApk(in, inner, log);
            Counter pos = new Counter(new BufferedOutputStream(new FileOutputStream(unsigned)));
            try (ZipFile z = new ZipFile(in); ZipOutputStream zo = new ZipOutputStream(pos)) {
                for (ZipEntry e : Collections.list(z.entries())) {
                    String n = e.getName();
                    if (n.startsWith("META-INF/") && (n.equals("META-INF/MANIFEST.MF") || n.matches("META-INF/[^/]+\\.(SF|RSA|DSA|EC)"))) continue;
                    if (n.equals(GUEST)) { put(zo, "lib/arm64-v8a/libguest.so", read(z, e)); continue; }
                    if (n.startsWith("lib/armeabi-v7a/") || n.startsWith("lib/x86/")) continue;
                    if (n.equals("AndroidManifest.xml")) { put(zo, n, patchManifest(read(z, e))); continue; }
                    if (n.equals("classes.dex")) { put(zo, n, patchDex(read(z, e))); continue; }
                    copy(z, e, zo, pos.n);
                }
                put(zo, "lib/arm64-v8a/libRecklessRacing2.so", bytes(loader));
                addObb(obb, zo, pos.n, log);
            }
            log.accept("Signing...");
            new ApkSigner.Builder(Collections.singletonList(
                    new ApkSigner.SignerConfig.Builder("CERT", key, Collections.singletonList(cert)).build()))
                    .setInputApk(unsigned).setOutputApk(out).setCreatedBy("RR2 Patcher").build().sign();
        } finally {
            unsigned.delete();
            inner.delete();
        }
    }

    /** The game APK, or the one inside an .xapk (a zip of the APK and its OBB, as mirror sites ship it). */
    static File unwrapApk(File in, File inner, Consumer<String> log) throws IOException {
        try (ZipFile z = new ZipFile(in)) {
            if (z.getEntry(GUEST) != null) return in;
            for (ZipEntry e : Collections.list(z.entries())) {
                if (!e.getName().endsWith(".apk")) continue;
                try (InputStream s = z.getInputStream(e); OutputStream o = new FileOutputStream(inner)) { pipe(s, o); }
                try (ZipFile apk = new ZipFile(inner)) {
                    if (apk.getEntry(GUEST) != null) { log.accept("Using the APK inside: " + e.getName()); return inner; }
                }
            }
        }
        throw new IOException("Not the original Reckless Racing 2 APK (no 32-bit game library)");
    }

    /** The picked OBB, or the .obb inside a zip of it (players often keep the obb folder zipped). */
    static void addObb(File obb, ZipOutputStream zo, long pos, Consumer<String> log) throws IOException {
        ZipFile wrap = null;
        try {
            try { wrap = new ZipFile(obb); } catch (ZipException notZip) {}
            ZipEntry inner = null;
            if (wrap != null)
                for (ZipEntry e : Collections.list(wrap.entries())) if (e.getName().endsWith(".obb")) inner = e;
            if (inner == null && wrap != null && wrap.getEntry("AndroidManifest.xml") != null)
                throw new IOException("Step 2 got an APK, not the game data (.obb)");
            if (inner != null) {
                log.accept("Adding game data from the zip: " + inner.getName());
                zo.putNextEntry(stored(OBB, inner.getSize(), inner.getCrc(), pos));
                try (InputStream s = wrap.getInputStream(inner)) { pipe(s, zo); }
            } else {
                log.accept("Adding game data...");
                CRC32 crc = new CRC32();
                try (InputStream s = new FileInputStream(obb)) {  // stored entries need their CRC up front
                    byte[] b = new byte[1 << 16];
                    for (int n; (n = s.read(b)) > 0; ) crc.update(b, 0, n);
                }
                zo.putNextEntry(stored(OBB, obb.length(), crc.getValue(), pos));
                try (InputStream s = new FileInputStream(obb)) { pipe(s, zo); }
            }
            zo.closeEntry();
        } finally {
            if (wrap != null) wrap.close();
        }
    }

    /** Byte patches to the Java code, then fix the dex header checksums. */
    static byte[] patchDex(byte[] d) throws Exception {
        for (String[] p : DEX_PATCHES) {
            byte[] old = hex(p[0]), rep = hex(p[1]);
            int at = -1;
            search:
            for (int i = 0; i + old.length <= d.length; i++) {
                for (int k = 0; k < old.length; k++) if (d[i + k] != old[k]) continue search;
                if (at >= 0) throw new IOException("classes.dex: patch site " + p[0] + " found twice");
                at = i;
            }
            if (at < 0) throw new IOException("classes.dex: patch site " + p[0] + " not found (different game version?)");
            System.arraycopy(rep, 0, d, at, rep.length);
        }
        MessageDigest sha = MessageDigest.getInstance("SHA-1");
        sha.update(d, 32, d.length - 32);
        System.arraycopy(sha.digest(), 0, d, 12, 20);
        Adler32 a = new Adler32();
        a.update(d, 12, d.length - 12);
        setI32(d, 8, (int) a.getValue());
        return d;
    }

    static byte[] hex(String h) {
        byte[] b = new byte[h.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(h.substring(2 * i, 2 * i + 2), 16);
        return b;
    }

    /** Binary AndroidManifest.xml: raise the targetSdkVersion integer in place, and move the
     *  <uses-sdk> elements to the top. The original has them after <application>, so Android sets up
     *  the activities with old-app defaults (letterboxed); apktool's rebuild reorders them the same way. */
    static byte[] patchManifest(byte[] m) throws IOException {
        int resMapStart = -1, resMapCount = 0, found = 0, top = -1;
        ByteArrayOutputStream sdk = new ByteArrayOutputStream(), rest = new ByteArrayOutputStream();
        for (int p = 8, size; p + 8 <= m.length && (size = i32(m, p + 4)) >= 8; p += size) {
            int type = u16(m, p), hdr = u16(m, p + 2);
            if (type == 0x0180) { resMapStart = p + hdr; resMapCount = (i32(m, p + 4) - hdr) / 4; }
            boolean usesSdk = false;
            if (type == 0x0102) {                              // start element
                if (top < 0) top = p + size;                   // end of <manifest ...>
                int ext = p + hdr, attrStart = u16(m, ext + 8), attrSize = u16(m, ext + 10), count = u16(m, ext + 12);
                for (int i = 0; i < count; i++) {
                    int a = ext + attrStart + i * attrSize, name = i32(m, a + 4);
                    int id = name >= 0 && name < resMapCount ? i32(m, resMapStart + name * 4) : 0;
                    if (id == ATTR_MIN_SDK) usesSdk = true;
                    if (id != ATTR_TARGET_SDK) continue;
                    usesSdk = true;
                    if ((m[a + 15] & 0xff) != 0x10) throw new IOException("targetSdkVersion is not a plain integer");
                    if (i32(m, a + 16) < MIN_TARGET_SDK) setI32(m, a + 16, MIN_TARGET_SDK);
                    found++;
                }
            }
            if (usesSdk) {                                     // the element and its end tag (no children)
                int end = p + size;
                if (u16(m, end) != 0x0103) throw new IOException("AndroidManifest.xml: unexpected uses-sdk layout");
                sdk.write(m, p, size + i32(m, end + 4));
                p = end;
                size = i32(m, end + 4);
            } else if (top >= 0 && p >= top) rest.write(m, p, size);
        }
        if (found != 1) throw new IOException("AndroidManifest.xml: expected one targetSdkVersion, found " + found);
        byte[] a = sdk.toByteArray(), b = rest.toByteArray();
        System.arraycopy(a, 0, m, top, a.length);
        System.arraycopy(b, 0, m, top + a.length, b.length);
        return m;
    }

    static int u16(byte[] b, int p) { return (b[p] & 0xff) | (b[p + 1] & 0xff) << 8; }
    static int i32(byte[] b, int p) { return u16(b, p) | u16(b, p + 2) << 16; }
    static void setI32(byte[] b, int p, int v) { for (int i = 0; i < 4; i++) b[p + i] = (byte) (v >> 8 * i); }

    static byte[] read(ZipFile z, ZipEntry e) throws IOException {
        try (InputStream s = z.getInputStream(e)) { return bytes(s); }
    }

    /** Tracks the zip writer's position, for alignment. */
    static final class Counter extends FilterOutputStream {
        long n;
        Counter(OutputStream o) { super(o); }
        @Override public void write(int b) throws IOException { out.write(b); n++; }
        @Override public void write(byte[] b, int off, int len) throws IOException { out.write(b, off, len); n += len; }
    }

    static void pipe(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }

    static byte[] bytes(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        pipe(in, b);
        return b.toByteArray();
    }

    static void put(ZipOutputStream zo, String name, byte[] data) throws IOException {
        zo.putNextEntry(new ZipEntry(name));
        zo.write(data);
        zo.closeEntry();
    }

    /** Uncompressed entry aligned like zipalign -p does (4 bytes, .so files 4096). pos = where its header goes. */
    static ZipEntry stored(String name, long size, long crc, long pos) throws IOException {
        ZipEntry o = new ZipEntry(name);
        o.setMethod(ZipEntry.STORED);
        o.setSize(size); o.setCompressedSize(size); o.setCrc(crc);
        // Android's alignment extra field (0xD935): id, size, alignment, then padding
        int align = name.endsWith(".so") ? 4096 : 4;
        long data = pos + 30 + name.getBytes("UTF-8").length + 6;
        byte[] x = new byte[6 + (int) ((align - data % align) % align)];
        x[0] = 0x35; x[1] = (byte) 0xD9; x[2] = (byte) (x.length - 4); x[3] = (byte) ((x.length - 4) >> 8);
        x[4] = (byte) align; x[5] = (byte) (align >> 8);
        o.setExtra(x);
        return o;
    }

    /** Unchanged entry; stored entries stay stored and aligned. */
    static void copy(ZipFile z, ZipEntry e, ZipOutputStream zo, long pos) throws IOException {
        zo.putNextEntry(e.getMethod() == ZipEntry.STORED ? stored(e.getName(), e.getSize(), e.getCrc(), pos)
                                                         : new ZipEntry(e.getName()));
        try (InputStream s = z.getInputStream(e)) { pipe(s, zo); }
        zo.closeEntry();
    }

    public static PrivateKey loadKey(InputStream pk8) throws Exception {
        return KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(bytes(pk8)));
    }

    public static X509Certificate loadCert(InputStream der) throws Exception {
        return (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(der);
    }

    /** PC test: Patcher original.apk game.obb loader.so key.pk8 cert.der out.apk */
    public static void main(String[] a) throws Exception {
        try (InputStream so = new FileInputStream(a[2]); InputStream k = new FileInputStream(a[3]); InputStream c = new FileInputStream(a[4])) {
            patch(new File(a[0]), new File(a[1]), so, new File(a[5]), loadKey(k), loadCert(c), System.out::println);
        }
        System.out.println("Wrote " + a[5]);
    }
}
