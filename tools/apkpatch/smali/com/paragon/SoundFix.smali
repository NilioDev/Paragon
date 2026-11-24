.class public Lcom/paragon/SoundFix;
.super Ljava/lang/Object;
.source "SoundFix.java"

# Remembers the looping track the game asked for, so it can be restarted after the app returns
# from the background (the original stops all sound in onPause and never restarts it).

.field private static active:Z
.field private static lastId:I
.field private static lastVol:I

.method public constructor <init>()V
    .locals 0
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static noteLoop(IIZ)V
    .locals 1
    if-eqz p2, :done
    sput p0, Lcom/paragon/SoundFix;->lastId:I
    sput p1, Lcom/paragon/SoundFix;->lastVol:I
    const/4 v0, 0x1
    sput-boolean v0, Lcom/paragon/SoundFix;->active:Z
    :done
    return-void
.end method

.method public static noteStop()V
    .locals 1
    const/4 v0, 0x0
    sput-boolean v0, Lcom/paragon/SoundFix;->active:Z
    return-void
.end method

.method public static resume()V
    .locals 3
    sget-boolean v0, Lcom/paragon/SoundFix;->active:Z
    if-eqz v0, :done
    sget v0, Lcom/paragon/SoundFix;->lastId:I
    sget v1, Lcom/paragon/SoundFix;->lastVol:I
    const/4 v2, 0x1
    :try_start
    invoke-static {v0, v1, v2}, Lcom/gamevil/nexus2/ui/NexusSound;->playSound(IIZ)V
    :try_end
    .catch Ljava/lang/Exception; {:try_start .. :try_end} :done
    :done
    return-void
.end method

# MediaPlayer completion: restart a track that should loop (setLooping alone is not reliable).
.method public static completed(Landroid/media/MediaPlayer;Z)Z
    .locals 1
    if-eqz p1, :no
    if-eqz p0, :no
    :try_start
    const/4 v0, 0x0
    invoke-virtual {p0, v0}, Landroid/media/MediaPlayer;->seekTo(I)V
    invoke-virtual {p0}, Landroid/media/MediaPlayer;->start()V
    :try_end
    .catch Ljava/lang/Exception; {:try_start .. :try_end} :no
    const/4 v0, 0x1
    return v0
    :no
    const/4 v0, 0x0
    return v0
.end method
