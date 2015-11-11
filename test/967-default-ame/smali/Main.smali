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
# class Main implements Iface, Iface2, Iface3 {
#   public static void main(String[] args) {
#       System.out.println("Create Main instance");
#       Main m = new Main();
#       System.out.println("Calling functions on concrete Main");
#       callMain(m);
#       System.out.println("Calling functions on interface Iface");
#       callIface(m);
#       System.out.println("Calling functions on interface Iface2");
#       callIface2(m);
#   }
#
#   public static void callMain(Main m) {
#       System.out.println("Calling non-abstract function on Main");
#       System.out.println(m.charge());
#       System.out.println("Calling abstract function on Main");
#       try {
#           System.out.println(m.sayHi());
#           System.out.println("Unexpected no error Thrown on Main");
#       } catch (AbstractMethodError e) {
#           System.out.println("Expected AME Thrown on Main");
#       } catch (IncompatibleClassChangeError e) {
#           System.out.println("Unexpected ICCE Thrown on Main");
#       }
#       System.out.println("Calling non-abstract function on Main");
#       System.out.println(m.charge());
#       return;
#   }
#
#   public static void callIface(Iface m) {
#       System.out.println("Calling non-abstract function on Iface");
#       System.out.println(m.charge());
#       System.out.println("Calling abstract function on Iface");
#       try {
#           System.out.println(m.sayHi());
#           System.out.println("Unexpected no error Thrown on Iface");
#       } catch (AbstractMethodError e) {
#           System.out.println("Expected AME Thrown on Iface");
#       } catch (IncompatibleClassChangeError e) {
#           System.out.println("Unexpected ICCE Thrown on Iface");
#       }
#       System.out.println("Calling non-abstract function on Iface");
#       System.out.println(m.charge());
#       return;
#   }
#
#   public static void callIface2(Iface2 m) {
#       System.out.println("Calling abstract function on Iface2");
#       try {
#           System.out.println(m.sayHi());
#           System.out.println("Unexpected no error Thrown on Iface2");
#       } catch (AbstractMethodError e) {
#           System.out.println("Expected AME Thrown on Iface2");
#       } catch (IncompatibleClassChangeError e) {
#           System.out.println("Unexpected ICCE Thrown on Iface2");
#       }
#       return;
#   }
# }

.class public LMain;
.super Ljava/lang/Object;
.implements LIface;
.implements LIface2;
.implements LIface3;

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

    const-string v0, "Calling functions on interface Iface2"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    invoke-static {v2}, LMain;->callIface2(LIface2;)V

    return-void
.end method

.method public static callIface(LIface;)V
    .locals 2
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v0, "Calling non-abstract function on Iface"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-interface {p0}, LIface;->charge()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    const-string v0, "Calling abstract function on Iface"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    :try_start
        invoke-interface {p0}, LIface;->sayHi()Ljava/lang/String;
        move-result-object v0
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

        const-string v0, "Unexpected no error Thrown on Iface"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

        goto :error_end
    :try_end
    .catch Ljava/lang/AbstractMethodError; {:try_start .. :try_end} :AME_error_start
    .catch Ljava/lang/IncompatibleClassChangeError; {:try_start .. :try_end} :ICCE_error_start
    :AME_error_start
        const-string v0, "Expected AME Thrown on Iface"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        goto :error_end
    :ICCE_error_start
        const-string v0, "Unexpected ICCE Thrown on Iface"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        goto :error_end
    :error_end
    const-string v0, "Calling non-abstract function on Iface"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-interface {p0}, LIface;->charge()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    return-void
.end method

.method public static callIface2(LIface2;)V
    .locals 2
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v0, "Calling abstract function on Iface2"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    :try_start
        invoke-interface {p0}, LIface2;->sayHi()Ljava/lang/String;
        move-result-object v0
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

        const-string v0, "Unexpected no error Thrown on Iface2"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

        goto :error_end
    :try_end
    .catch Ljava/lang/AbstractMethodError; {:try_start .. :try_end} :AME_error_start
    .catch Ljava/lang/IncompatibleClassChangeError; {:try_start .. :try_end} :ICCE_error_start
    :AME_error_start
        const-string v0, "Expected AME Thrown on Iface2"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        goto :error_end
    :ICCE_error_start
        const-string v0, "Unexpected ICCE Thrown on Iface2"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        goto :error_end
    :error_end

    return-void
.end method

.method public static callMain(LMain;)V
    .locals 2
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v0, "Calling non-abstract function on Main"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-virtual {p0}, LMain;->charge()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    const-string v0, "Calling abstract function on Main"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    :try_start
        invoke-virtual {p0}, LMain;->sayHi()Ljava/lang/String;
        move-result-object v0
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

        const-string v0, "Unexpected no error Thrown on Main"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

        goto :error_end
    :try_end
    .catch Ljava/lang/AbstractMethodError; {:try_start .. :try_end} :AME_error_start
    .catch Ljava/lang/IncompatibleClassChangeError; {:try_start .. :try_end} :ICCE_error_start
    :AME_error_start
        const-string v0, "Expected AME Thrown on Main"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        goto :error_end
    :ICCE_error_start
        const-string v0, "Unexpected ICCE Thrown on Main"
        invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
        goto :error_end
    :error_end
    const-string v0, "Calling non-abstract function on Main"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-virtual {p0}, LMain;->charge()Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    return-void
.end method
