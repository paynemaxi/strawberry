/*
 * Strawberry Music Player
 * Copyright 2026, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "version.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonParseError>
#include <QJsonDocument>
#include <QJsonValue>
#include <QJsonObject>
#include <QJsonArray>

#include "core/networkaccessmanager.h"
#include "core/taskmanager.h"
#include "core/iconloader.h"
#include "radiobrowserservice.h"
#include "radiochannel.h"

using namespace Qt::Literals::StringLiterals;

namespace {
constexpr char kApiCountriesUrl[] = "https://de1.api.radio-browser.info/json/countries";
constexpr char kApiStatesUrl[] = "https://de1.api.radio-browser.info/json/states";
constexpr char kApiStationSearchUrl[] = "https://de1.api.radio-browser.info/json/stations/search";
constexpr char kOthersState[] = "__others__";
}  // namespace

RadioBrowserService::RadioBrowserService(const SharedPtr<TaskManager> task_manager, const SharedPtr<NetworkAccessManager> network, QObject *parent)
    : RadioService(Song::Source::RadioBrowser, u"Radio Browser"_s, IconLoader::Load(u"radio"_s), task_manager, network, parent) {}

RadioBrowserService::~RadioBrowserService() {
  Abort();
}

QUrl RadioBrowserService::Homepage() { return QUrl(u"https://www.radio-browser.info/"_s); }
QUrl RadioBrowserService::Donate() { return QUrl(u"https://www.radio-browser.info/"_s); }

void RadioBrowserService::Abort() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
  }

  requested_states_.clear();
  requested_stations_.clear();

}

void RadioBrowserService::GetChannels() {

  Abort();

  QUrl url(QString::fromLatin1(kApiCountriesUrl));
  QUrlQuery url_query;
  url_query.addQueryItem(u"order"_s, u"name"_s);
  url.setQuery(url_query);
  GetJsonArray(url, tr("Getting %1 countries").arg(name_), [this](QNetworkReply *reply, const int task_id) { GetCountriesReply(reply, task_id); });

}

void RadioBrowserService::GetStates(const QString &country) {

  const QString key = country.trimmed();
  if (requested_states_.contains(key)) return;
  requested_states_.insert(key);

  QUrl url(QString::fromLatin1(kApiStatesUrl) + u'/' + QString::fromLatin1(QUrl::toPercentEncoding(country)));
  QUrlQuery url_query;
  url_query.addQueryItem(u"order"_s, u"name"_s);
  url.setQuery(url_query);
  GetJsonArray(url, tr("Getting %1 states").arg(name_), [this, country](QNetworkReply *reply, const int task_id) { GetStatesReply(reply, task_id, country); });

}

void RadioBrowserService::GetStations(const QString &country, const QString &state) {

  const QString key = country + u'\n' + state;
  if (requested_stations_.contains(key)) return;
  requested_stations_.insert(key);

  QUrl url(QString::fromLatin1(kApiStationSearchUrl));
  QUrlQuery url_query;
  url_query.addQueryItem(u"country"_s, country);
  url_query.addQueryItem(u"countryExact"_s, u"true"_s);
  if (state == QLatin1String(kOthersState)) {
    url_query.addQueryItem(u"state"_s, QString());
    url_query.addQueryItem(u"stateExact"_s, u"true"_s);
  }
  else {
    url_query.addQueryItem(u"state"_s, state);
    url_query.addQueryItem(u"stateExact"_s, u"true"_s);
  }
  url_query.addQueryItem(u"order"_s, u"name"_s);
  url.setQuery(url_query);
  GetJsonArray(url, tr("Getting %1 channels").arg(name_), [this, country, state](QNetworkReply *reply, const int task_id) { GetStationsReply(reply, task_id, country, state); });

}

void RadioBrowserService::GetJsonArray(const QUrl &url, const QString &task_name, const std::function<void(QNetworkReply*, int)> &finished) {

  QNetworkRequest network_request(url);
  network_request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Strawberry/%1").arg(QLatin1String(STRAWBERRY_VERSION_DISPLAY)));
  QNetworkReply *reply = network_->get(network_request);
  replies_ << reply;
  const int task_id = task_manager_->StartTask(task_name);
  QObject::connect(reply, &QNetworkReply::finished, this, [reply, task_id, finished]() { finished(reply, task_id); });

}

QJsonArray RadioBrowserService::ExtractJsonArray(QNetworkReply *reply) {

  if (replies_.contains(reply)) replies_.removeAll(reply);
  reply->deleteLater();

  const QByteArray data = ExtractData(reply);
  if (data.isEmpty()) {
    return QJsonArray();
  }

  QJsonParseError json_error;
  const QJsonDocument json_document = QJsonDocument::fromJson(data, &json_error);

  if (json_error.error != QJsonParseError::NoError) {
    Error(QStringLiteral("Failed to parse Json data from %1: %2").arg(name_, json_error.errorString()));
    return QJsonArray();
  }

  if (!json_document.isArray()) {
    Error(u"Json document is not an array."_s, json_document);
    return QJsonArray();
  }

  return json_document.array();

}

void RadioBrowserService::GetCountriesReply(QNetworkReply *reply, const int task_id) {

  QStringList countries;
  const QJsonArray array_countries = ExtractJsonArray(reply);
  for (const QJsonValue &value_country : array_countries) {
    if (!value_country.isObject()) continue;
    const QJsonObject obj_country = value_country.toObject();
    const QString name = obj_country["name"_L1].toString().trimmed();
    if (!name.isEmpty()) countries << name;
  }

  countries.removeDuplicates();
  countries.sort(Qt::CaseInsensitive);

  task_manager_->SetTaskFinished(task_id);
  Q_EMIT NewCountries(countries);

}

void RadioBrowserService::GetStatesReply(QNetworkReply *reply, const int task_id, const QString &country) {

  QStringList states;
  const QJsonArray array_states = ExtractJsonArray(reply);
  for (const QJsonValue &value_state : array_states) {
    if (!value_state.isObject()) continue;
    const QJsonObject obj_state = value_state.toObject();
    states << obj_state["name"_L1].toString().trimmed();
  }

  states.removeDuplicates();
  states.sort(Qt::CaseInsensitive);

  task_manager_->SetTaskFinished(task_id);
  Q_EMIT NewStates(country, states);

}

void RadioBrowserService::GetStationsReply(QNetworkReply *reply, const int task_id, const QString &country, const QString &state) {

  RadioChannelList channels;
  const QJsonArray array_stations = ExtractJsonArray(reply);
  for (const QJsonValue &value_station : array_stations) {
    if (!value_station.isObject()) continue;
    const QJsonObject obj_station = value_station.toObject();

    const QString name = obj_station["name"_L1].toString().trimmed();
    QString url = obj_station["url_resolved"_L1].toString().trimmed();
    if (url.isEmpty()) url = obj_station["url"_L1].toString().trimmed();
    if (name.isEmpty() || url.isEmpty()) continue;

    RadioChannel channel;
    channel.source = source_;
    channel.name = name;

    const QString station_country = obj_station["country"_L1].toString().trimmed();
    const QString country_code = obj_station["countrycode"_L1].toString().trimmed();
    channel.country = !station_country.isEmpty() ? station_country : (!country.isEmpty() ? country : country_code);
    channel.state = obj_station["state"_L1].toString().trimmed();
    if (state == QLatin1String(kOthersState)) {
      channel.state = QString::fromLatin1(kOthersState);
    }
    else if (channel.state.isEmpty()) {
      channel.state = state;
    }

    const QString codec = obj_station["codec"_L1].toString().trimmed();
    const int bitrate = obj_station["bitrate"_L1].toInt();
    QStringList details;
    if (!codec.isEmpty()) details << codec;
    if (bitrate > 0) details << QStringLiteral("%1 kbps").arg(bitrate);
    if (!details.isEmpty()) channel.name += u" ("_s + details.join(u", "_s) + u")"_s;

    channel.url.setUrl(url);
    channel.thumbnail_url.setUrl(obj_station["favicon"_L1].toString().trimmed());
    channels << channel;
  }

  task_manager_->SetTaskFinished(task_id);
  Q_EMIT NewChannels(channels);

}
