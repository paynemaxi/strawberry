/*
 * Strawberry Music Player
 * Copyright 2021, Jonas Kvinge <jonas@jkvinge.net>
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

#include <memory>

#include <QObject>
#include <QSortFilterProxyModel>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/database.h"
#include "core/networkaccessmanager.h"
#include "radioservices.h"
#include "radiobackend.h"
#include "radiomodel.h"
#include "radioservice.h"
#include "radiochannel.h"
#include "somafmservice.h"
#include "radioparadiseservice.h"
#include "radiobrowserservice.h"

using std::make_shared;

RadioServices::RadioServices(const SharedPtr<TaskManager> task_manager,
                             const SharedPtr<NetworkAccessManager> network,
                             const SharedPtr<Database> database,
                             const SharedPtr<AlbumCoverLoader> albumcover_loader,
                             QObject *parent)
    : QObject(parent),
      network_(network),
      backend_(nullptr),
      model_(new RadioModel(albumcover_loader, SharedPtr<RadioServices>(this))),
      sort_model_(new QSortFilterProxyModel(this)),
      channels_refresh_(false) {

  backend_ = make_shared<RadioBackend>(database);
  backend_->moveToThread(database->thread());

  QObject::connect(&*backend_, &RadioBackend::NewChannels, this, &RadioServices::GotChannelsFromBackend);
  QObject::connect(&*backend_, &RadioBackend::RadioBrowserCountries, this, &RadioServices::GotRadioBrowserCountriesFromBackend);

  sort_model_->setSourceModel(model_);
  sort_model_->setSortRole(RadioModel::Role_SortText);
  sort_model_->setDynamicSortFilter(true);
  sort_model_->setSortLocaleAware(true);
  sort_model_->sort(0);

  AddService(new SomaFMService(task_manager, network_, this));
  AddService(new RadioParadiseService(task_manager, network_, this));
  AddService(new RadioBrowserService(task_manager, network_, this));

}

void RadioServices::AddService(RadioService *service) {

  qLog(Debug) << "Adding radio service:" << service->name();
  services_.insert(service->source(), service);

  QObject::connect(service, &RadioService::NewChannels, this, &RadioServices::GotChannelsFromService);
  if (RadioBrowserService *radio_browser_service = qobject_cast<RadioBrowserService*>(service)) {
    QObject::connect(radio_browser_service, &RadioBrowserService::NewCountries, this, &RadioServices::GotRadioBrowserCountries);
    QObject::connect(radio_browser_service, &RadioBrowserService::NewStates, this, &RadioServices::GotRadioBrowserStates);
  }
  QObject::connect(service, &RadioService::destroyed, this, &RadioServices::ServiceDeleted);

}

void RadioServices::RemoveService(RadioService *service) {

  if (!services_.contains(service->source())) return;

  services_.remove(service->source());
  QObject::disconnect(service, nullptr, this, nullptr);

}

void RadioServices::ServiceDeleted() {

  RadioService *service = qobject_cast<RadioService*>(sender());
  if (service) RemoveService(service);

}

RadioService *RadioServices::ServiceBySource(const Song::Source source) const {

  if (services_.contains(source)) return services_.value(source);
  return nullptr;

}

void RadioServices::ReloadSettings() {

  const QList<RadioService*> services = services_.values();
  for (RadioService *service : services) {
    service->ReloadSettings();
  }

}

void RadioServices::GetChannels() {

  model_->Reset();
  AddServicesToModel();
  backend_->GetRadioBrowserCountriesAsync();
  backend_->GetChannelsAsync();

}

void RadioServices::RefreshChannels() {

  channels_refresh_ = true;
  model_->Reset();
  AddServicesToModel();
  backend_->DeleteChannelsAsync();
  backend_->DeleteRadioBrowserCountriesAsync();

  const QList<RadioService*> services = services_.values();
  for (RadioService *service : services) {
    service->GetChannels();
  }

}

void RadioServices::GetRadioBrowserCountries() {

  if (RadioBrowserService *radio_browser_service = qobject_cast<RadioBrowserService*>(ServiceBySource(Song::Source::RadioBrowser))) {
    radio_browser_service->GetChannels();
  }

}

void RadioServices::GetRadioBrowserStates(const QString &country) {

  if (RadioBrowserService *service = qobject_cast<RadioBrowserService*>(ServiceBySource(Song::Source::RadioBrowser))) {
    service->GetStates(country);
  }

}

void RadioServices::GetRadioBrowserStations(const QString &country, const QString &state) {

  if (RadioBrowserService *service = qobject_cast<RadioBrowserService*>(ServiceBySource(Song::Source::RadioBrowser))) {
    service->GetStations(country, state);
  }

}

void RadioServices::AddServicesToModel() {

  const QList<RadioService*> services = services_.values();
  for (RadioService *service : services) {
    model_->AddService(service->source());
  }

}

void RadioServices::GotChannelsFromBackend(const RadioChannelList &channels) {

  if (channels.isEmpty()) {
    if (!channels_refresh_) {
      RefreshChannels();
    }
  }
  else {
    model_->AddChannels(channels);
  }

}

void RadioServices::GotChannelsFromService(const RadioChannelList &channels) {

  RadioService *service = qobject_cast<RadioService*>(sender());
  if (!service) return;

  if (service->source() == Song::Source::RadioBrowser) {
    model_->AddChannels(channels);
    return;
  }

  backend_->AddChannelsAsync(channels);

}

void RadioServices::GotRadioBrowserCountriesFromBackend(const QStringList &countries) {

  if (countries.isEmpty()) {
    if (!channels_refresh_) {
      GetRadioBrowserCountries();
    }
  }
  else {
    model_->AddRadioBrowserCountries(countries);
  }

}

void RadioServices::GotRadioBrowserCountries(const QStringList &countries) {

  model_->AddRadioBrowserCountries(countries);
  backend_->AddRadioBrowserCountriesAsync(countries);

}

void RadioServices::GotRadioBrowserStates(const QString &country, const QStringList &states) {

  model_->AddRadioBrowserStates(country, states);

}
