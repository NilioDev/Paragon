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

# Gapless looping: MediaPlayer.setLooping leaves an audible pause at the loop point on current
# Android versions, so a looping clip keeps a second, prepared player chained with
# setNextMediaPlayer and the two are swapped on completion.

.method public static armLoop(Lcom/gamevil/nexus2/ui/AudioClip;)V
    .locals 4
    monitor-enter p0
    :try_start
    iget-boolean v0, p0, Lcom/gamevil/nexus2/ui/AudioClip;->mLoop:Z
    if-eqz v0, :done
    iget-object v0, p0, Lcom/gamevil/nexus2/ui/AudioClip;->mPlayer:Landroid/media/MediaPlayer;
    if-eqz v0, :done
    iget-object v1, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pNext:Landroid/media/MediaPlayer;
    if-nez v1, :done
    iget-object v1, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pCtx:Landroid/content/Context;
    iget v2, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pResId:I
    invoke-static {v1, v2}, Landroid/media/MediaPlayer;->create(Landroid/content/Context;I)Landroid/media/MediaPlayer;
    move-result-object v1
    if-nez v1, :chain
    const/4 v2, 0x1
    invoke-virtual {v0, v2}, Landroid/media/MediaPlayer;->setLooping(Z)V
    goto :done
    :chain
    iget-object v2, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pListener:Landroid/media/MediaPlayer$OnCompletionListener;
    invoke-virtual {v1, v2}, Landroid/media/MediaPlayer;->setOnCompletionListener(Landroid/media/MediaPlayer$OnCompletionListener;)V
    invoke-virtual {v0, v1}, Landroid/media/MediaPlayer;->setNextMediaPlayer(Landroid/media/MediaPlayer;)V
    iput-object v1, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pNext:Landroid/media/MediaPlayer;
    :try_end
    .catch Ljava/lang/Exception; {:try_start .. :try_end} :failed
    .catchall {:try_start .. :try_end} :unlock
    :done
    monitor-exit p0
    return-void
    :failed
    move-exception v3
    monitor-exit p0
    return-void
    :unlock
    move-exception v3
    monitor-exit p0
    throw v3
.end method

# from AudioClip.release(): drop the prepared follow-up player
.method public static dropNext(Lcom/gamevil/nexus2/ui/AudioClip;)V
    .locals 2
    iget-object v0, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pNext:Landroid/media/MediaPlayer;
    if-eqz v0, :done
    const/4 v1, 0x0
    iput-object v1, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pNext:Landroid/media/MediaPlayer;
    :try_start
    invoke-virtual {v0}, Landroid/media/MediaPlayer;->release()V
    :try_end
    .catch Ljava/lang/Exception; {:try_start .. :try_end} :done
    :done
    return-void
.end method

# completion listener: true while the clip keeps looping (the chained player has already started)
.method public static advance(Lcom/gamevil/nexus2/ui/AudioClip;Landroid/media/MediaPlayer;)Z
    .locals 4
    const/4 v3, 0x0
    monitor-enter p0
    :try_start
    iget-boolean v0, p0, Lcom/gamevil/nexus2/ui/AudioClip;->mLoop:Z
    if-eqz v0, :done
    iget-object v0, p0, Lcom/gamevil/nexus2/ui/AudioClip;->mPlayer:Landroid/media/MediaPlayer;
    if-ne v0, p1, :done
    iget-object v1, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pNext:Landroid/media/MediaPlayer;
    if-eqz v1, :restart
    iput-object v1, p0, Lcom/gamevil/nexus2/ui/AudioClip;->mPlayer:Landroid/media/MediaPlayer;
    const/4 v2, 0x0
    iput-object v2, p0, Lcom/gamevil/nexus2/ui/AudioClip;->pNext:Landroid/media/MediaPlayer;
    invoke-virtual {p1}, Landroid/media/MediaPlayer;->release()V
    invoke-static {p0}, Lcom/paragon/SoundFix;->armLoop(Lcom/gamevil/nexus2/ui/AudioClip;)V
    const/4 v3, 0x1
    goto :done
    :restart
    const/4 v2, 0x0
    invoke-virtual {p1, v2}, Landroid/media/MediaPlayer;->seekTo(I)V
    invoke-virtual {p1}, Landroid/media/MediaPlayer;->start()V
    const/4 v3, 0x1
    :try_end
    .catch Ljava/lang/Exception; {:try_start .. :try_end} :done
    .catchall {:try_start .. :try_end} :unlock
    :done
    monitor-exit p0
    return v3
    :unlock
    move-exception v0
    monitor-exit p0
    throw v0
.end method
