.class public LsameFieldNames;
.super Ljava/lang/Object;

# Test multiple fields with the same name and different types.
# (Invalid in Java language but valid in bytecode.)
.field static public a:D
.field static public a:S
.field static public a:J
.field static public a:F
.field static public a:Z
.field static public a:I
.field static public a:B
.field static public a:C
.field static public a:Ljava/lang/Integer;
.field static public a:Ljava/lang/Long;
.field static public a:Ljava/lang/Float;
.field static public a:Ljava/lang/Double;
.field static public a:Ljava/lang/Boolean;
.field static public a:Ljava/lang/Void;
.field static public a:Ljava/lang/Short;
.field static public a:Ljava/lang/Char;
.field static public a:Ljava/lang/Byte;

.method public static getInt()I
    .locals 2
    const/4 v0, 2
    sput v0, LsameFieldNames;->a:I
    sget-object v1, LsameFieldNames;->a:Ljava/lang/Integer;
    const/4 v1, 0
    if-nez v1, :fail
    const/4 v0, 7
    :ret
    return v0
    :fail
    const/4 v0, 0
    goto :ret
.end method
