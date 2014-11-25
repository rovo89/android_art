.class public LPackedSwitch;

.super Ljava/lang/Object;

.method public static packedSwitch(I)I
    .registers 2

    const/4 v0, 0
    packed-switch v0, :switch_data
    goto :default

    :switch_data
    .packed-switch 0x0
        :case
    .end packed-switch

    :return
    return v1

    :default
    goto :return

    :case
    goto :return

.end method
