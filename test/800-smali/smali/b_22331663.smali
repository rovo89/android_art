.class public LB22331663;
.super Ljava/lang/Object;


.method public static run(Z)V
.registers 6
       # Make v4 defined, just use null.
       const v4, 0

       if-eqz v5, :Label2

:Label1
       # Construct a java.lang.Object completely, and throw a new exception.
       new-instance v4, Ljava/lang/Object;
       invoke-direct {v4}, Ljava/lang/Object;-><init>()V

       new-instance v3, Ljava/lang/RuntimeException;
       invoke-direct {v3}, Ljava/lang/RuntimeException;-><init>()V
       throw v3

:Label2
       # Allocate a java.lang.Object (do not initialize), and throw a new exception.
       new-instance v4, Ljava/lang/Object;

       new-instance v3, Ljava/lang/RuntimeException;
       invoke-direct {v3}, Ljava/lang/RuntimeException;-><init>()V
       throw v3

:Label3
       # Catch handler. Here we had to merge the uninitialized with the initialized reference,
       # which creates a conflict. Copy the conflict, and then return. This should not make the
       # verifier fail the method.
       move-object v0, v4

       return-void

.catchall {:Label1 .. :Label3} :Label3
.end method
