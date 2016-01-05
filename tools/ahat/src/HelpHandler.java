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

import com.google.common.io.ByteStreams;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintStream;

/**
 * HelpHandler.
 *
 * HttpHandler to show the help page.
 */
class HelpHandler implements HttpHandler {

  @Override
  public void handle(HttpExchange exchange) throws IOException {
    ClassLoader loader = HelpHandler.class.getClassLoader();
    exchange.getResponseHeaders().add("Content-Type", "text/html;charset=utf-8");
    exchange.sendResponseHeaders(200, 0);
    PrintStream ps = new PrintStream(exchange.getResponseBody());
    HtmlDoc doc = new HtmlDoc(ps, DocString.text("ahat"), DocString.uri("style.css"));
    doc.menu(Menu.getMenu());

    InputStream is = loader.getResourceAsStream("help.html");
    if (is == null) {
      ps.println("No help available.");
    } else {
      ByteStreams.copy(is, ps);
    }

    doc.close();
    ps.close();
  }
}
