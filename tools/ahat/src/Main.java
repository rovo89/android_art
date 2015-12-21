/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.ahat;

import com.sun.net.httpserver.HttpServer;
import java.io.File;
import java.io.IOException;
import java.io.PrintStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.concurrent.Executors;

public class Main {

  public static void help(PrintStream out) {
    out.println("java -jar ahat.jar [-p port] FILE");
    out.println("  Launch an http server for viewing "
        + "the given Android heap-dump FILE.");
    out.println("");
    out.println("Options:");
    out.println("  -p <port>");
    out.println("     Serve pages on the given port. Defaults to 7100.");
    out.println("");
  }

  public static void main(String[] args) throws IOException {
    int port = 7100;
    for (String arg : args) {
      if (arg.equals("--help")) {
        help(System.out);
        return;
      }
    }

    File hprof = null;
    for (int i = 0; i < args.length; i++) {
      if ("-p".equals(args[i]) && i + 1 < args.length) {
        i++;
        port = Integer.parseInt(args[i]);
      } else {
        if (hprof != null) {
          System.err.println("multiple input files.");
          help(System.err);
          return;
        }
        hprof = new File(args[i]);
      }
    }

    if (hprof == null) {
      System.err.println("no input file.");
      help(System.err);
      return;
    }

    System.out.println("Processing hprof file...");
    AhatSnapshot ahat = AhatSnapshot.fromHprof(hprof);

    InetAddress loopback = InetAddress.getLoopbackAddress();
    InetSocketAddress addr = new InetSocketAddress(loopback, port);
    HttpServer server = HttpServer.create(addr, 0);
    server.createContext("/", new AhatHttpHandler(new OverviewHandler(ahat, hprof)));
    server.createContext("/rooted", new AhatHttpHandler(new RootedHandler(ahat)));
    server.createContext("/object", new AhatHttpHandler(new ObjectHandler(ahat)));
    server.createContext("/objects", new AhatHttpHandler(new ObjectsHandler(ahat)));
    server.createContext("/site", new AhatHttpHandler(new SiteHandler(ahat)));
    server.createContext("/native", new AhatHttpHandler(new NativeAllocationsHandler(ahat)));
    server.createContext("/bitmap", new BitmapHandler(ahat));
    server.createContext("/help", new HelpHandler());
    server.createContext("/style.css", new StaticHandler("style.css", "text/css"));
    server.setExecutor(Executors.newFixedThreadPool(1));
    System.out.println("Server started on localhost:" + port);
    server.start();
  }
}

