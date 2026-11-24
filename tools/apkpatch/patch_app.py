#!/usr/bin/env python3
"""Patch an apktool-decoded Zenonia 1.2.0 APK (Java side + manifest) for modern Android.

Usage: patch_app.py DECODED_DIR

- targetSdk 34 / minSdk 24 (the 64-bit native library needs API 24+)
- launcher activity exported, Play Store package visibility for the licence check
- licence service bind gets an explicit package (implicit service intents are rejected since Android 5)
- IMEI lookup replaced by ANDROID_ID (getDeviceId throws on Android 10+)
- org.apache.http.legacy declared (used by the licence policy)
The Google Play licence check itself is kept.
"""
import os
import re
import sys

d = sys.argv[1]


def edit(path, fn):
    p = os.path.join(d, path)
    s = open(p, encoding='utf-8').read()
    s2 = fn(s)
    if s2 == s:
        raise SystemExit('no change applied to ' + path)
    open(p, 'w', encoding='utf-8').write(s2)


def manifest(s):
    s = s.replace('    <uses-permission android:name="android.permission.READ_PHONE_STATE"/>\n', '')
    s = s.replace('<application ', '<queries>\n        <package android:name="com.android.vending"/>\n    </queries>\n    <application ', 1)
    s = s.replace('android:debuggable="false"', 'android:debuggable="false" android:resizeableActivity="false" '
                  'android:usesCleartextTraffic="true"', 1)
    s = s.replace('android:configChanges="keyboardHidden|orientation"',
                  'android:configChanges="keyboard|keyboardHidden|navigation|orientation|screenLayout|screenSize|'
                  'smallestScreenSize|uiMode|density" android:exported="true"')
    s = s.replace('        <meta-data android:name="xperiaplayoptimized_content"',
                  '        <uses-library android:name="org.apache.http.legacy" android:required="false"/>\n'
                  '        <meta-data android:name="xperiaplayoptimized_content"', 1)
    return s


def apktool_yml(s):
    s = re.sub(r'  minSdkVersion: \d+\n(  targetSdkVersion: \d+\n)?', '  minSdkVersion: 24\n  targetSdkVersion: 34\n', s)
    return s


def launcher(s):
    old = """    sget-object v1, Lcom/gamevil/nexus2/NexusGLActivity;->myActivity:Lcom/gamevil/nexus2/NexusGLActivity;

    const-string v2, "phone"

    invoke-virtual {v1, v2}, Lcom/gamevil/nexus2/NexusGLActivity;->getSystemService(Ljava/lang/String;)Ljava/lang/Object;

    move-result-object v1

    check-cast v1, Landroid/telephony/TelephonyManager;

    invoke-virtual {v1}, Landroid/telephony/TelephonyManager;->getDeviceId()Ljava/lang/String;
"""
    new = """    invoke-virtual {p0}, Lcom/gamevil/zenonia/ZenoniaLouncher;->getContentResolver()Landroid/content/ContentResolver;

    move-result-object v1

    const-string v2, "android_id"

    invoke-static {v1, v2}, Landroid/provider/Settings$Secure;->getString(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;
"""
    return s.replace(old, new)


def licence(s):
    old = '    invoke-direct {v2, v3}, Landroid/content/Intent;-><init>(Ljava/lang/String;)V\n'
    return s.replace(old, old + '\n    const-string v3, "com.android.vending"\n\n'
                     '    invoke-virtual {v2, v3}, Landroid/content/Intent;->setPackage(Ljava/lang/String;)Landroid/content/Intent;\n', 1)


edit('AndroidManifest.xml', manifest)
edit('apktool.yml', apktool_yml)
edit('smali/com/gamevil/zenonia/ZenoniaLouncher.smali', launcher)
edit('smali/com/android/vending/licensing/LicenseChecker.smali', licence)
print('patched', d)
