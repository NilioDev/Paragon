#!/usr/bin/env python3
"""Patch an apktool-decoded Zenonia 1.2.0 APK (Java side + manifest) for modern Android.

Usage: patch_app.py DECODED_DIR

- targetSdk 34 / minSdk 24 (the 64-bit native library needs API 24+)
- launcher activity exported, Play Store package visibility for the licence check
- licence service bind gets an explicit package (implicit service intents are rejected since Android 5)
- IMEI lookup replaced by ANDROID_ID (getDeviceId throws on Android 10+)
- org.apache.http.legacy declared (used by the licence policy)
- sound: gapless looping (chained MediaPlayers); the running track restarts after returning from the background
- skip button hit area scaled to the screen (was a fixed 70x70 pixels)
- widescreen: com.paragon.Wide picks the game width from the display; HUD touch areas follow the centred HUD
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


def resume(s):
    old = '    invoke-super {p0}, Landroid/app/Activity;->onResume()V\n'
    return s.replace(old, old + '\n    invoke-static {p0}, Lcom/paragon/Wide;->init(Landroid/app/Activity;)V\n'
                     '\n    invoke-static {p0}, Lcom/paragon/Immersive;->apply(Landroid/app/Activity;)V\n', 1)


def oncreate(s):
    old = '    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V\n'
    return s.replace(old, old + '\n    invoke-static {p0}, Lcom/paragon/Immersive;->apply(Landroid/app/Activity;)V\n', 1)


def after_prologue(s, header, code):
    i = s.index(header)
    j = s.index('    .prologue\n', i) + len('    .prologue\n')
    return s[:j] + code + s[j:]


def sound_play(s):
    return after_prologue(s, '.method public static playSound(IIZ)V\n',
                          '    invoke-static {p0, p1, p2}, Lcom/paragon/SoundFix;->noteLoop(IIZ)V\n\n')


def sound_stop(s):
    return after_prologue(s, '.method public OnStopSound()V\n', '    invoke-static {}, Lcom/paragon/SoundFix;->noteStop()V\n\n')


def sound_resume(s):
    old = '    invoke-static {}, Lcom/gamevil/nexus2/Natives;->InitializeJNIGlobalRef()V\n'
    return s.replace(old, old + '\n    invoke-static {}, Lcom/paragon/SoundFix;->resume()V\n', 1)


AC = 'Lcom/gamevil/nexus2/ui/AudioClip;'
MP = 'Landroid/media/MediaPlayer;'
LST = 'Landroid/media/MediaPlayer$OnCompletionListener;'


def clip_fields(s):
    s = s.replace('.field private mLoop:Z', '.field public mLoop:Z', 1)
    s = s.replace('.field private mPlayer:' + MP, '.field public mPlayer:' + MP + '\n\n.field public pNext:' + MP + '\n\n'
                  '.field public pCtx:Landroid/content/Context;\n\n.field public pResId:I\n\n.field public pListener:' + LST, 1)
    # constructor: remember context, resource and completion listener for the chained loop player
    old = '    invoke-direct {p0}, Ljava/lang/Object;-><init>()V\n'
    s = s.replace(old, old + '\n    iput-object p1, p0, ' + AC + '->pCtx:Landroid/content/Context;\n\n'
                  '    iput p2, p0, ' + AC + '->pResId:I\n', 1)
    old = '    invoke-virtual {v1, v2}, ' + MP + '->setOnCompletionListener(' + LST + ')V\n'
    s = s.replace(old, old + '\n    iput-object v2, p0, ' + AC + '->pListener:' + LST + '\n', 1)
    # setLooping: chain a second player instead of MediaPlayer.setLooping
    old = '    const/4 v1, 0x1\n\n    invoke-virtual {v0, v1}, ' + MP + '->setLooping(Z)V\n'
    if old not in s:
        raise SystemExit('setLooping pattern not found')
    s = s.replace(old, '    invoke-static {p0}, Lcom/paragon/SoundFix;->armLoop(' + AC + ')V\n', 1)
    s = after_prologue(s, '.method public release()V\n', '    invoke-static {p0}, Lcom/paragon/SoundFix;->dropNext(' + AC + ')V\n\n')
    return s


def clip_completion(s):
    old = '''    iget-object v0, p0, Lcom/gamevil/nexus2/ui/AudioClip$1;->this$0:Lcom/gamevil/nexus2/ui/AudioClip;
'''
    new = old + '''
    invoke-static {v0, p1}, Lcom/paragon/SoundFix;->advance(Lcom/gamevil/nexus2/ui/AudioClip;Landroid/media/MediaPlayer;)Z

    move-result v1

    if-eqz v1, :paragon_stop

    return-void

    :paragon_stop
'''
    return s.replace(old, new, 1)


def skip_button(s):
    """The skip hit area was 70x70 screen pixels, sized for 480x320 phones; scale it with the screen."""
    ui = 'Lcom/gamevil/nexus2/ui/UIControllerView;'
    old = """    const/16 v2, 0x46

    .line 38
    sget v0, %s->width:I

    sub-int/2addr v0, v2

    const/4 v1, 0x0

    invoke-virtual {p0, v0, v1, v2, v2}, Lcom/gamevil/zenonia/ui/UISkipButton;->setPosition(IIII)V
""" % ui
    new = """    sget v0, %s->width:I

    mul-int/lit8 v2, v0, 0x46

    div-int/lit16 v2, v2, 0x1e0

    sget v1, %s->height:I

    mul-int/lit8 v1, v1, 0x46

    div-int/lit16 v1, v1, 0x140

    sub-int/2addr v0, v2

    const/4 v3, 0x0

    invoke-virtual {p0, v0, v3, v2, v1}, Lcom/gamevil/zenonia/ui/UISkipButton;->setPosition(IIII)V
""" % (ui, ui)
    s = s.replace('.method public initialize()V\n    .locals 3\n', '.method public initialize()V\n    .locals 4\n', 1)
    return s.replace(old, new, 1)


def hud_touch(cls, *conv):
    """Touch areas over controls the game draws itself: replace getConvertedX(reg) by a Wide helper.
    conv: (register, helper) pairs."""
    def fn(s):
        call = '    invoke-virtual {p0, %s}, Lcom/gamevil/zenonia/ui/' + cls + ';->getConvertedX(I)I\n'
        for reg, helper in conv:
            old = call % reg
            if old not in s:
                raise SystemExit('getConvertedX call not found in ' + cls)
            s = s.replace(old, '    invoke-static {%s}, Lcom/paragon/Wide;->%s(I)I\n' % (reg, helper), 1)
        return s
    return fn

import shutil
shutil.copytree(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'smali'), os.path.join(d, 'smali'), dirs_exist_ok=True)
edit('smali/com/gamevil/nexus2/NexusGLActivity.smali', resume)
edit('smali/com/gamevil/nexus2/NexusGLActivity.smali', oncreate)
edit('AndroidManifest.xml', manifest)
edit('apktool.yml', apktool_yml)
edit('smali/com/gamevil/zenonia/ZenoniaLouncher.smali', launcher)
edit('smali/com/android/vending/licensing/LicenseChecker.smali', licence)
edit('smali/com/gamevil/nexus2/ui/NexusSound.smali', sound_play)
edit('smali/com/gamevil/zenonia/ui/ZenoniaUIControllerView.smali', sound_stop)
edit('smali/com/gamevil/nexus2/NexusGLActivity.smali', sound_resume)
edit('smali/com/gamevil/nexus2/ui/AudioClip.smali', clip_fields)
edit('smali/com/gamevil/nexus2/ui/AudioClip$1.smali', clip_completion)
edit('smali/com/gamevil/zenonia/ui/UISkipButton.smali', skip_button)
edit('smali/com/gamevil/zenonia/ui/UIQuickSlot.smali', hud_touch('UIQuickSlot', ('v0', 'uiX480'), ('v2', 'uiW480')))
edit('smali/com/gamevil/zenonia/ui/UIQuickSlotSwapButton.smali', hud_touch('UIQuickSlotSwapButton', ('v0', 'uiX480'), ('v2', 'uiW480')))
edit('smali/com/gamevil/zenonia/ui/UISelectButton.smali', hud_touch('UISelectButton', ('v0', 'uiR480'), ('v3', 'uiW480')))
edit('smali/com/gamevil/zenonia/ui/UIDirectionPad.smali', hud_touch('UIDirectionPad', ('v3', 'uiW480')))
print('patched', d)
