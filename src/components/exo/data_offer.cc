// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/exo/data_offer.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/i18n/icu_string_conversions.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/pickle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/task/thread_pool.h"
#include "components/exo/data_device.h"
#include "components/exo/data_exchange_delegate.h"
#include "components/exo/data_offer_delegate.h"
#include "components/exo/data_offer_observer.h"
#include "net/base/filename_util.h"
#include "third_party/skia/include/core/SkEncodedImageFormat.h"
#include "third_party/skia/include/core/SkImageEncoder.h"
#include "third_party/skia/include/core/SkStream.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/clipboard_constants.h"
#include "ui/base/clipboard/file_info.h"
#include "ui/base/data_transfer_policy/data_transfer_endpoint.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "url/gurl.h"

namespace exo {
namespace {

constexpr char kTextMimeTypeUtf16[] = "text/plain;charset=utf-16";
constexpr char kTextHtmlMimeTypeUtf8[] = "text/html;charset=utf-8";
constexpr char kTextHtmlMimeTypeUtf16[] = "text/html;charset=utf-16";
constexpr char kTextRtfMimeType[] = "text/rtf";
constexpr char kImagePngMimeType[] = "image/png";

constexpr char kUTF8[] = "utf8";
constexpr char kUTF16[] = "utf16";

void WriteFileDescriptorOnWorkerThread(
    base::ScopedFD fd,
    scoped_refptr<base::RefCountedMemory> memory) {
  if (!base::WriteFileDescriptor(fd.get(),
                                 reinterpret_cast<const char*>(memory->front()),
                                 memory->size()))
    DLOG(ERROR) << "Failed to write drop data";
}

void WriteFileDescriptor(base::ScopedFD fd,
                         scoped_refptr<base::RefCountedMemory> memory) {
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&WriteFileDescriptorOnWorkerThread, std::move(fd),
                     std::move(memory)));
}

ui::ClipboardFormatType GetClipboardFormatType() {
  static const char kFormatString[] = "chromium/x-file-system-files";
  static base::NoDestructor<ui::ClipboardFormatType> format_type(
      ui::ClipboardFormatType::GetType(kFormatString));
  return *format_type;
}

scoped_refptr<base::RefCountedString> EncodeAsRefCountedString(
    const std::u16string& text,
    const std::string& charset) {
  std::string encoded_text;
  base::UTF16ToCodepage(text, charset.c_str(),
                        base::OnStringConversionError::SUBSTITUTE,
                        &encoded_text);
  return base::RefCountedString::TakeString(&encoded_text);
}

DataOffer::AsyncSendDataCallback AsyncEncodeAsRefCountedString(
    const std::u16string& text,
    const std::string& charset) {
  return base::BindOnce(
      [](const std::u16string& text, const std::string& charset,
         DataOffer::SendDataCallback callback) {
        std::move(callback).Run(EncodeAsRefCountedString(text, charset));
      },
      text, charset);
}

void ReadTextFromClipboard(const std::string& charset,
                           const ui::DataTransferEndpoint data_dst,
                           DataOffer::SendDataCallback callback) {
  std::u16string text;
  ui::Clipboard::GetForCurrentThread()->ReadText(
      ui::ClipboardBuffer::kCopyPaste, &data_dst, &text);
  std::move(callback).Run(EncodeAsRefCountedString(text, charset));
}

void ReadHTMLFromClipboard(const std::string& charset,
                           const ui::DataTransferEndpoint data_dst,
                           DataOffer::SendDataCallback callback) {
  std::u16string text;
  std::string url;
  uint32_t start, end;
  ui::Clipboard::GetForCurrentThread()->ReadHTML(
      ui::ClipboardBuffer::kCopyPaste, &data_dst, &text, &url, &start, &end);
  std::move(callback).Run(EncodeAsRefCountedString(text, charset));
}

void ReadRTFFromClipboard(const ui::DataTransferEndpoint data_dst,
                          DataOffer::SendDataCallback callback) {
  std::string text;
  ui::Clipboard::GetForCurrentThread()->ReadRTF(ui::ClipboardBuffer::kCopyPaste,
                                                &data_dst, &text);
  std::move(callback).Run(base::RefCountedString::TakeString(&text));
}

scoped_refptr<base::RefCountedMemory> EncodePNGOnWorkerThread(
    const SkBitmap& sk_bitmap) {
  SkDynamicMemoryWStream data_stream;
  if (SkEncodeImage(&data_stream, sk_bitmap.pixmap(),
                    SkEncodedImageFormat::kPNG, 100)) {
    std::vector<uint8_t> data(data_stream.bytesWritten());
    data_stream.copyToAndReset(data.data());
    return base::RefCountedBytes::TakeVector(&data);
  } else {
    LOG(ERROR) << "Couldn't encode image as PNG";
    return nullptr;
  }
}

void OnReceivePNGFromClipboard(DataOffer::SendDataCallback callback,
                               const SkBitmap& sk_bitmap) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&EncodePNGOnWorkerThread, std::move(sk_bitmap)),
      base::BindOnce(
          [](DataOffer::SendDataCallback callback,
             scoped_refptr<base::RefCountedMemory> data) {
            std::move(callback).Run(data);
          },
          std::move(callback)));
}

void ReadPNGFromClipboard(const ui::DataTransferEndpoint data_dst,
                          DataOffer::SendDataCallback callback) {
  ui::Clipboard::GetForCurrentThread()->ReadImage(
      ui::ClipboardBuffer::kCopyPaste, &data_dst,
      base::BindOnce(&OnReceivePNGFromClipboard, std::move(callback)));
}

}  // namespace

ScopedDataOffer::ScopedDataOffer(DataOffer* data_offer,
                                 DataOfferObserver* observer)
    : data_offer_(data_offer), observer_(observer) {
  data_offer_->AddObserver(observer_);
}

ScopedDataOffer::~ScopedDataOffer() {
  data_offer_->RemoveObserver(observer_);
}

DataOffer::DataOffer(DataOfferDelegate* delegate)
    : delegate_(delegate), dnd_action_(DndAction::kNone), finished_(false) {}

DataOffer::~DataOffer() {
  delegate_->OnDataOfferDestroying(this);
  for (DataOfferObserver& observer : observers_) {
    observer.OnDataOfferDestroying(this);
  }
}

void DataOffer::AddObserver(DataOfferObserver* observer) {
  observers_.AddObserver(observer);
}

void DataOffer::RemoveObserver(DataOfferObserver* observer) {
  observers_.RemoveObserver(observer);
}

void DataOffer::Accept(const std::string* mime_type) {}

void DataOffer::Receive(const std::string& mime_type, base::ScopedFD fd) {
  const auto callbacks_it = data_callbacks_.find(mime_type);
  if (callbacks_it != data_callbacks_.end()) {
    // Set cache with empty data to indicate in process.
    DCHECK(data_cache_.count(mime_type) == 0);
    data_cache_.emplace(mime_type, nullptr);
    std::move(callbacks_it->second)
        .Run(base::BindOnce(&DataOffer::OnDataReady,
                            weak_ptr_factory_.GetWeakPtr(), mime_type,
                            std::move(fd)));
    data_callbacks_.erase(callbacks_it);
    return;
  }

  const auto cache_it = data_cache_.find(mime_type);
  if (cache_it == data_cache_.end()) {
    DLOG(ERROR) << "Unexpected mime type is requested " << mime_type;
    return;
  }

  if (cache_it->second) {
    WriteFileDescriptor(std::move(fd), cache_it->second);
  } else {
    // Data bytes for this mime type are being processed currently.
    pending_receive_requests_.push_back(
        std::make_pair(mime_type, std::move(fd)));
  }
}

void DataOffer::Finish() {
  finished_ = true;
}

void DataOffer::SetActions(const base::flat_set<DndAction>& dnd_actions,
                           DndAction preferred_action) {
  dnd_action_ = preferred_action;
  delegate_->OnAction(preferred_action);
}

void DataOffer::SetSourceActions(
    const base::flat_set<DndAction>& source_actions) {
  source_actions_ = source_actions;
  delegate_->OnSourceActions(source_actions);
}

void DataOffer::SetDropData(DataExchangeDelegate* data_exchange_delegate,
                            aura::Window* target,
                            const ui::OSExchangeData& data) {
  DCHECK_EQ(0u, data_callbacks_.size());

  ui::EndpointType endpoint_type =
      data_exchange_delegate->GetDataTransferEndpointType(target);
  const std::string uri_list_mime_type =
      data_exchange_delegate->GetMimeTypeForUriList(endpoint_type);
  if (data.HasFile()) {
    std::vector<ui::FileInfo> files;
    if (data.GetFilenames(&files)) {
      data_callbacks_.emplace(
          uri_list_mime_type,
          base::BindOnce(&DataExchangeDelegate::SendFileInfo,
                         base::Unretained(data_exchange_delegate),
                         endpoint_type, std::move(files)));
      delegate_->OnOffer(uri_list_mime_type);
      return;
    }
  }

  base::Pickle pickle;
  if (data.GetPickledData(GetClipboardFormatType(), &pickle) &&
      data_exchange_delegate->HasUrlsInPickle(pickle)) {
    data_callbacks_.emplace(
        uri_list_mime_type,
        base::BindOnce(&DataExchangeDelegate::SendPickle,
                       base::Unretained(data_exchange_delegate), endpoint_type,
                       pickle));
    delegate_->OnOffer(uri_list_mime_type);
    return;
  }

  base::FilePath file_contents_filename;
  std::string file_contents;
  if (data.provider().HasFileContents() &&
      data.provider().GetFileContents(&file_contents_filename,
                                      &file_contents)) {
    std::string filename = file_contents_filename.value();
    base::ReplaceChars(filename, "\\", "\\\\", &filename);
    base::ReplaceChars(filename, "\"", "\\\"", &filename);
    const std::string mime_type =
        base::StrCat({"application/octet-stream;name=\"", filename, "\""});
    auto callback = base::BindOnce(
        [](scoped_refptr<base::RefCountedString> contents,
           DataOffer::SendDataCallback callback) {
          std::move(callback).Run(std::move(contents));
        },
        base::RefCountedString::TakeString(&file_contents));
    data_callbacks_.emplace(mime_type, std::move(callback));
    delegate_->OnOffer(mime_type);
  }

  std::u16string string_content;
  if (data.HasString() && data.GetString(&string_content)) {
    const std::string utf8_mime_type = std::string(ui::kMimeTypeTextUtf8);
    data_callbacks_.emplace(
        utf8_mime_type, AsyncEncodeAsRefCountedString(string_content, kUTF8));
    delegate_->OnOffer(utf8_mime_type);
    const std::string utf16_mime_type = std::string(kTextMimeTypeUtf16);
    data_callbacks_.emplace(
        utf16_mime_type, AsyncEncodeAsRefCountedString(string_content, kUTF16));
    delegate_->OnOffer(utf16_mime_type);
    const std::string text_plain_mime_type = std::string(ui::kMimeTypeText);
    // The MIME type standard says that new text/ subtypes should default to a
    // UTF-8 encoding, but that old ones, including text/plain, keep ASCII as
    // the default. Nonetheless, we use UTF8 here because it is a superset of
    // ASCII and the defacto standard text encoding.
    data_callbacks_.emplace(text_plain_mime_type, AsyncEncodeAsRefCountedString(
                                                      string_content, kUTF8));
    delegate_->OnOffer(text_plain_mime_type);
  }

  std::u16string html_content;
  GURL url_content;
  if (data.HasHtml() && data.GetHtml(&html_content, &url_content)) {
    const std::string utf8_html_mime_type = std::string(kTextHtmlMimeTypeUtf8);
    data_callbacks_.emplace(utf8_html_mime_type,
                            AsyncEncodeAsRefCountedString(html_content, kUTF8));
    delegate_->OnOffer(utf8_html_mime_type);

    const std::string utf16_html_mime_type =
        std::string(kTextHtmlMimeTypeUtf16);
    data_callbacks_.emplace(utf16_html_mime_type, AsyncEncodeAsRefCountedString(
                                                      html_content, kUTF16));
    delegate_->OnOffer(utf16_html_mime_type);
  }
}

void DataOffer::SetClipboardData(DataExchangeDelegate* data_exchange_delegate,
                                 const ui::Clipboard& data,
                                 ui::EndpointType endpoint_type) {
  DCHECK_EQ(0u, data_callbacks_.size());
  const ui::DataTransferEndpoint data_dst(endpoint_type);
  if (data.IsFormatAvailable(ui::ClipboardFormatType::GetPlainTextType(),
                             ui::ClipboardBuffer::kCopyPaste, &data_dst)) {
    auto utf8_callback = base::BindRepeating(&ReadTextFromClipboard,
                                             std::string(kUTF8), data_dst);
    delegate_->OnOffer(std::string(ui::kMimeTypeTextUtf8));
    data_callbacks_.emplace(std::string(ui::kMimeTypeTextUtf8), utf8_callback);
    delegate_->OnOffer(std::string(ui::kMimeTypeLinuxUtf8String));
    data_callbacks_.emplace(std::string(ui::kMimeTypeLinuxUtf8String),
                            utf8_callback);
    delegate_->OnOffer(std::string(kTextMimeTypeUtf16));
    data_callbacks_.emplace(
        std::string(kTextMimeTypeUtf16),
        base::BindOnce(&ReadTextFromClipboard, std::string(kUTF16), data_dst));
  }
  if (data.IsFormatAvailable(ui::ClipboardFormatType::GetHtmlType(),
                             ui::ClipboardBuffer::kCopyPaste, &data_dst)) {
    delegate_->OnOffer(std::string(kTextHtmlMimeTypeUtf8));
    data_callbacks_.emplace(
        std::string(kTextHtmlMimeTypeUtf8),
        base::BindOnce(&ReadHTMLFromClipboard, std::string(kUTF8), data_dst));
    delegate_->OnOffer(std::string(kTextHtmlMimeTypeUtf16));
    data_callbacks_.emplace(
        std::string(kTextHtmlMimeTypeUtf16),
        base::BindOnce(&ReadHTMLFromClipboard, std::string(kUTF16), data_dst));
  }
  if (data.IsFormatAvailable(ui::ClipboardFormatType::GetRtfType(),
                             ui::ClipboardBuffer::kCopyPaste, &data_dst)) {
    delegate_->OnOffer(std::string(kTextRtfMimeType));
    data_callbacks_.emplace(std::string(kTextRtfMimeType),
                            base::BindOnce(&ReadRTFFromClipboard, data_dst));
  }
  if (data.IsFormatAvailable(ui::ClipboardFormatType::GetBitmapType(),
                             ui::ClipboardBuffer::kCopyPaste, &data_dst)) {
    delegate_->OnOffer(std::string(kImagePngMimeType));
    data_callbacks_.emplace(std::string(kImagePngMimeType),
                            base::BindOnce(&ReadPNGFromClipboard, data_dst));
  }

  // We accept the filenames pickle from FilesApp, or text/uri-list from apps.
  std::vector<ui::FileInfo> filenames =
      data_exchange_delegate->ParseClipboardFilenamesPickle(endpoint_type,
                                                            data);
  if (filenames.empty() &&
      data.IsFormatAvailable(ui::ClipboardFormatType::GetFilenamesType(),
                             ui::ClipboardBuffer::kCopyPaste, &data_dst)) {
    data.ReadFilenames(ui::ClipboardBuffer::kCopyPaste, &data_dst, &filenames);
  }
  if (!filenames.empty()) {
    delegate_->OnOffer(std::string(ui::kMimeTypeURIList));
    data_callbacks_.emplace(
        std::string(ui::kMimeTypeURIList),
        base::BindOnce(&DataExchangeDelegate::SendFileInfo,
                       base::Unretained(data_exchange_delegate), endpoint_type,
                       std::move(filenames)));
  }
}

void DataOffer::OnDataReady(const std::string& mime_type,
                            base::ScopedFD fd,
                            scoped_refptr<base::RefCountedMemory> data) {
  // Update cache from nullptr to data.
  const auto cache_it = data_cache_.find(mime_type);
  DCHECK(cache_it != data_cache_.end());
  DCHECK(!cache_it->second);
  data_cache_.erase(cache_it);
  data_cache_.emplace(mime_type, data);

  WriteFileDescriptor(std::move(fd), data);

  // Process pending receive requests for this mime type, if there are any.
  auto it = pending_receive_requests_.begin();
  while (it != pending_receive_requests_.end()) {
    if (it->first == mime_type) {
      WriteFileDescriptor(std::move(it->second), data);
      it = pending_receive_requests_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace exo
