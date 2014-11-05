.class public LB17978759;
.super Ljava/lang/Object;

  .method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
  .end method

  .method public test()V
    .registers 2

    move-object   v0, p0
    # v0 and p0 alias
    monitor-enter p0
    # monitor-enter on p0
    monitor-exit  v0
    # monitor-exit on v0, however, verifier doesn't track this and so this is
    # a warning. Verifier will still think p0 is locked.

    move-object   v0, p0
    # v0 will now appear locked.
    monitor-enter v0
    # Attempt to lock v0 twice is a verifier failure.
    monitor-exit  v0

    return-void
  .end method
