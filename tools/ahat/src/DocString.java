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

import com.google.common.html.HtmlEscapers;
import java.net.URI;
import java.net.URISyntaxException;

/**
 * A class representing a small string of document content consisting of text,
 * links, images, etc.
 */
class DocString {
  private StringBuilder mStringBuilder;

  public DocString() {
    mStringBuilder = new StringBuilder();
  }

  /**
   * Construct a new DocString, initialized with the given text.
   */
  public static DocString text(String str) {
    DocString doc = new DocString();
    return doc.append(str);
  }

  /**
   * Construct a new DocString, initialized with the given formatted text.
   */
  public static DocString format(String format, Object... args) {
    DocString doc = new DocString();
    return doc.appendFormat(format, args);
  }

  /**
   * Construct a new DocString, initialized with the given link.
   */
  public static DocString link(URI uri, DocString content) {
    DocString doc = new DocString();
    return doc.appendLink(uri, content);

  }

  /**
   * Construct a new DocString initialized with the given image.
   */
  public static DocString image(URI uri, String alt) {
    return (new DocString()).appendImage(uri, alt);
  }

  /**
   * Append literal text to the given doc string.
   * Returns this object.
   */
  public DocString append(String text) {
    mStringBuilder.append(HtmlEscapers.htmlEscaper().escape(text));
    return this;
  }

  /**
   * Append formatted text to the given doc string.
   * Returns this object.
   */
  public DocString appendFormat(String format, Object... args) {
    append(String.format(format, args));
    return this;
  }

  public DocString append(DocString str) {
    mStringBuilder.append(str.html());
    return this;
  }

  public DocString appendLink(URI uri, DocString content) {
    mStringBuilder.append("<a href=\"");
    mStringBuilder.append(uri.toASCIIString());
    mStringBuilder.append("\">");
    mStringBuilder.append(content.html());
    mStringBuilder.append("</a>");
    return this;
  }

  public DocString appendImage(URI uri, String alt) {
    mStringBuilder.append("<img alt=\"");
    mStringBuilder.append(HtmlEscapers.htmlEscaper().escape(alt));
    mStringBuilder.append("\" src=\"");
    mStringBuilder.append(uri.toASCIIString());
    mStringBuilder.append("\" />");
    return this;
  }

  public DocString appendThumbnail(URI uri, String alt) {
    mStringBuilder.append("<img height=\"16\" alt=\"");
    mStringBuilder.append(HtmlEscapers.htmlEscaper().escape(alt));
    mStringBuilder.append("\" src=\"");
    mStringBuilder.append(uri.toASCIIString());
    mStringBuilder.append("\" />");
    return this;
  }

  /**
   * Convenience function for constructing a URI from a string with a uri
   * known to be valid.
   */
  public static URI uri(String uriString) {
    try {
      return new URI(uriString);
    } catch (URISyntaxException e) {
      throw new IllegalStateException("Known good uri has syntax error: " + uriString, e);
    }
  }

  /**
   * Convenience function for constructing a URI from a formatted string with
   * a uri known to be valid.
   */
  public static URI formattedUri(String format, Object... args) {
    return uri(String.format(format, args));
  }

  /**
   * Render the DocString as html.
   */
  public String html() {
    return mStringBuilder.toString();
  }
}
