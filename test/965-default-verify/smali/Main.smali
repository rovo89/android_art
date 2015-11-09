# /*
#  * Copyright (C) 2015 The Android Open Source Project
#  *
#  * Licensed under the Apache License, Version 2.0 (the "License");
#  * you may not use this file except in compliance with the License.
#  * You may obtain a copy of the License at
#  *
#  *      http://www.apache.org/licenses/LICENSE-2.0
#  *
#  * Unless required by applicable law or agreed to in writing, software
#  * distributed under the License is distributed on an "AS IS" BASIS,
#  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  * See the License for the specific language governing permissions and
#  * limitations under the License.
#  */
#
# class Main implements Iface {
#   public static void main(String[] args) {
#       System.out.println("Create Main instance");
#       Main m = new Main();
#       System.out.println("Calling functions on concrete Main");
#       callMain(m);
#       System.out.println("Calling functions on interface Iface");
#       callIface(m);
#   }
#
#   public static void callMain(Main m) {
#       System.out.println("Calling verifiable function on Main");
#       System.out.println(m.sayHi());
#       System.out.println("Calling unverifiable function on Main");
#       try {
#           m.verificationSoftFail();
#           System.out.println("Unexpected no error Thrown on Main");
#       } catch (NoSuchMethodError e) {
#           System.out.println("Expected NSME Thrown on Main");
#       } catch (Throwable e) {
#           System.out.println("Unexpected Error Thrown on Main");
#           e.printStackTrace(System.out);
#       }
#       System.out.println("Calling verifiable function on Main");
#       System.out.println(m.sayHi());
#       return;
#   }
#
#   public static void callIface(Iface m) {
#       System.out.println("Calling verifiable function on Iface");
#       System.out.println(m.sayHi());
#       System.out.println("Calling unverifiable function on Iface");
#       try {
#           m.verificationSoftFail();
#           System.out.println("Unexpected no error Thrown on Iface");
#       } catch (NoSuchMethodError e) {
#           System.out.println("Expected NSME Thrown on Iface");
#       } catch (Throwable e) {
#           System.out.println("Unexpected Error Thrown on Iface");
#           e.printStackTrace(System.out);
#       }
#       System.out.println("Calling verifiable function on Iface");
#       System.out.println(m.sayHi());
#       return;
#   }
# }

.class public LMain;
.super Ljava/lang/Object;
.implements LIface;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static main([Ljava/lang/String;)V
    .locals 3
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;

    const-string v0, "Create Main instance"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    new-instance v2, LMain;
    invoke-direct {v2}, LMain;-><init>()V

    const-string v0, "Calling functions on concrete Main"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    invoke-static {v2}, LMain;->callMain(LMain;)V

    const-string v0, "Calling functions on interface Iface"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    invoke-static {v2}, LMain;->callIface(LIface;)V

    return-void
.end method

.method public static callIface(LIface;)V
    .locals 3
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v0, "Calling verifiable function on Iface"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-interface {p0}, LIface;->sayHi()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    const-string v0, "Calling unverifiable function on Iface"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    :try_start
        invoke-interface {p0}, LIface;->verificationSoftFail()V

        const-string v0, "Unexpected no error Thrown on Iface"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

        goto :error_end
    :try_end
    .catch Ljava/lang/NoSuchMethodError; {:try_start .. :try_end} :NSME_error_start
    .catch Ljava/lang/Throwable; {:try_start .. :try_end} :other_error_start
    :NSME_error_start
        const-string v0, "Expected NSME Thrown on Iface"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        goto :error_end
    :other_error_start
        move-exception v2
        const-string v0, "Unexpected Error Thrown on Iface"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        invoke-virtual {v2,v1}, Ljava/lang/Throwable;->printStackTrace(Ljava/io/PrintStream;)V
        goto :error_end
    :error_end
    const-string v0, "Calling verifiable function on Iface"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-interface {p0}, LIface;->sayHi()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    return-void
.end method

.method public static callMain(LMain;)V
    .locals 3
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v0, "Calling verifiable function on Main"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-virtual {p0}, LMain;->sayHi()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    const-string v0, "Calling unverifiable function on Main"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    :try_start
        invoke-virtual {p0}, LMain;->verificationSoftFail()V

        const-string v0, "Unexpected no error Thrown on Main"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

        goto :error_end
    :try_end
    .catch Ljava/lang/NoSuchMethodError; {:try_start .. :try_end} :NSME_error_start
    .catch Ljava/lang/Throwable; {:try_start .. :try_end} :other_error_start
    :NSME_error_start
        const-string v0, "Expected NSME Thrown on Main"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        goto :error_end
    :other_error_start
        move-exception v2
        const-string v0, "Unexpected Error Thrown on Main"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        invoke-virtual {v2,v1}, Ljava/lang/Throwable;->printStackTrace(Ljava/io/PrintStream;)V
        goto :error_end
    :error_end
    const-string v0, "Calling verifiable function on Main"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-virtual {p0}, LMain;->sayHi()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    return-void
.end method
