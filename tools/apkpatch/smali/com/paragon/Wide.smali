.class public Lcom/paragon/Wide;
.super Ljava/lang/Object;
.source "Wide.java"

# Widescreen support.  gameW is the width of the game's framebuffer (height is always 240), chosen
# from the display's aspect ratio; the native library reads it when the game starts.  The game's HUD
# stays a 320-pixel layout centred in that framebuffer, so touch areas over HUD elements are mapped
# with uiX480/uiW480 instead of the plain 480-pixel scaling.

.field public static gameW:I

.method static constructor <clinit>()V
    .locals 1
    const/16 v0, 0x140
    sput v0, Lcom/paragon/Wide;->gameW:I
    return-void
.end method

.method public constructor <init>()V
    .locals 0
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static init(Landroid/app/Activity;)V
    .locals 5
    :try_start
    invoke-virtual {p0}, Landroid/app/Activity;->getWindowManager()Landroid/view/WindowManager;
    move-result-object v0
    invoke-interface {v0}, Landroid/view/WindowManager;->getDefaultDisplay()Landroid/view/Display;
    move-result-object v0
    new-instance v1, Landroid/graphics/Point;
    invoke-direct {v1}, Landroid/graphics/Point;-><init>()V
    invoke-virtual {v0, v1}, Landroid/view/Display;->getRealSize(Landroid/graphics/Point;)V
    iget v2, v1, Landroid/graphics/Point;->x:I
    iget v3, v1, Landroid/graphics/Point;->y:I
    if-ge v2, v3, :ordered
    move v4, v2
    move v2, v3
    move v3, v4
    :ordered
    if-lez v3, :done
    # w = 240 * long / short, even, clamped to 320..640
    mul-int/lit16 v2, v2, 0xf0
    div-int/2addr v2, v3
    and-int/lit8 v2, v2, -0x2
    const/16 v4, 0x140
    if-ge v2, v4, :notsmall
    move v2, v4
    :notsmall
    const/16 v4, 0x280
    if-le v2, v4, :notbig
    move v2, v4
    :notbig
    sput v2, Lcom/paragon/Wide;->gameW:I
    :try_end
    .catch Ljava/lang/Exception; {:try_start .. :try_end} :done
    :done
    return-void
.end method

# x in the original 480-pixel layout -> screen pixels, for HUD elements drawn by the game
.method public static uiX480(I)I
    .locals 3
    sget v0, Lcom/paragon/Wide;->gameW:I
    add-int/lit16 v1, v0, -0x140
    mul-int/lit8 v1, v1, 0x3
    div-int/lit8 v1, v1, 0x2
    mul-int/lit8 v2, p0, 0x2
    add-int/2addr v1, v2
    sget v2, Lcom/gamevil/nexus2/ui/UIControllerView;->width:I
    mul-int/2addr v1, v2
    mul-int/lit8 v0, v0, 0x3
    div-int/2addr v1, v0
    return v1
.end method

# a width in the original 480-pixel layout -> screen pixels
.method public static uiW480(I)I
    .locals 3
    sget v0, Lcom/paragon/Wide;->gameW:I
    mul-int/lit8 v1, p0, 0x2
    sget v2, Lcom/gamevil/nexus2/ui/UIControllerView;->width:I
    mul-int/2addr v1, v2
    mul-int/lit8 v0, v0, 0x3
    div-int/2addr v1, v0
    return v1
.end method

# x in the original 480-pixel layout, for controls anchored to the right edge of the screen
.method public static uiR480(I)I
    .locals 3
    sget v0, Lcom/paragon/Wide;->gameW:I
    rsub-int v1, p0, 0x1e0
    mul-int/lit8 v1, v1, 0x2
    sget v2, Lcom/gamevil/nexus2/ui/UIControllerView;->width:I
    mul-int/2addr v1, v2
    mul-int/lit8 v0, v0, 0x3
    div-int/2addr v1, v0
    sub-int v1, v2, v1
    return v1
.end method
