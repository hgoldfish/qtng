#ifndef QTNG_QT_BRIDGE_CONVERT_H
#define QTNG_QT_BRIDGE_CONVERT_H

#include <QtCore/qbytearray.h>
#include <QtCore/qlist.h>
#include <QtCore/qmap.h>
#include <QtCore/qstring.h>
#include <QtCore/qurl.h>
#include <QtCore/qurlquery.h>
#include <QtCore/qvariant.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define qtng qtng_core
#include "qtng/utils/url.h"
#undef qtng

namespace qtng_bridge {

inline std::string toStdString(const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

inline std::string toStdString(const std::string &s)
{
    return s;
}

inline QString toQString(const std::string &s)
{
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

inline std::string toStdString(const QByteArray &b)
{
    return std::string(b.constData(), static_cast<size_t>(b.size()));
}

inline QByteArray toQByteArray(const std::string &s)
{
    return QByteArray(s.data(), static_cast<int>(s.size()));
}

inline qtng_core::utils::Url toCoreUrl(const QUrl &url)
{
    return qtng_core::utils::Url(url.toString(QUrl::FullyEncoded).toUtf8().constData());
}

inline QUrl toQUrl(const qtng_core::utils::Url &url)
{
    return QUrl::fromEncoded(QByteArray::fromStdString(url.toString()));
}

inline qtng_core::utils::UrlQuery toCoreUrlQuery(const QUrlQuery &query)
{
    qtng_core::utils::UrlQuery result;
    const QList<QPair<QString, QString>> items = query.queryItems();
    for (const QPair<QString, QString> &item : items) {
        result.addQueryItem(toStdString(item.first), toStdString(item.second));
    }
    return result;
}

inline QUrlQuery toQUrlQuery(const qtng_core::utils::UrlQuery &query)
{
    QUrlQuery result;
    for (const auto &item : query.items()) {
        result.addQueryItem(toQString(item.first), toQString(item.second));
    }
    return result;
}

template<typename T, typename U>
QList<T> toQList(const std::vector<U> &v, T (*convert)(const U &))
{
    QList<T> result;
    result.reserve(static_cast<int>(v.size()));
    for (const U &item : v) {
        result.append(convert(item));
    }
    return result;
}

template<typename T, typename U>
std::vector<U> toStdVector(const QList<T> &list, U (*convert)(const T &))
{
    std::vector<U> result;
    result.reserve(static_cast<size_t>(list.size()));
    for (const T &item : list) {
        result.push_back(convert(item));
    }
    return result;
}

template<typename T, typename U>
QMap<QString, T> toQMap(const std::map<std::string, U> &m, T (*convert)(const U &))
{
    QMap<QString, T> result;
    for (const auto &item : m) {
        result.insert(toQString(item.first), convert(item.second));
    }
    return result;
}

template<typename T, typename U>
std::map<std::string, U> toStdMap(const QMap<QString, T> &m, U (*convert)(const T &))
{
    std::map<std::string, U> result;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        result.emplace(toStdString(it.key()), convert(it.value()));
    }
    return result;
}

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_CONVERT_H
