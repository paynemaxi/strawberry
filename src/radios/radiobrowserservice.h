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

#ifndef RADIOBROWSERSERVICE_H
#define RADIOBROWSERSERVICE_H

#include <functional>

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "radioservice.h"
#include "radiochannel.h"

class TaskManager;
class NetworkAccessManager;
class QNetworkReply;

class RadioBrowserService : public RadioService {
  Q_OBJECT

 public:
  explicit RadioBrowserService(const SharedPtr<TaskManager> task_manager, const SharedPtr<NetworkAccessManager> network, QObject *parent = nullptr);
  ~RadioBrowserService();

  QUrl Homepage() override;
  QUrl Donate() override;

  void Abort();

 public Q_SLOTS:
  void GetChannels() override;
  void GetStates(const QString &country);
  void GetStations(const QString &country, const QString &state);

 Q_SIGNALS:
  void NewCountries(const QStringList &countries);
  void NewStates(const QString &country, const QStringList &states);

 private Q_SLOTS:
  void GetCountriesReply(QNetworkReply *reply, const int task_id);
  void GetStatesReply(QNetworkReply *reply, const int task_id, const QString &country);
  void GetStationsReply(QNetworkReply *reply, const int task_id, const QString &country, const QString &state);

 private:
  void GetJsonArray(const QUrl &url, const QString &task_name, const std::function<void(QNetworkReply*, int)> &finished);
  QJsonArray ExtractJsonArray(QNetworkReply *reply);

  QList<QNetworkReply*> replies_;
  QSet<QString> requested_states_;
  QSet<QString> requested_stations_;
};

#endif  // RADIOBROWSERSERVICE_H
