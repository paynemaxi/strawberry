ALTER TABLE radio_channels ADD COLUMN country TEXT;

ALTER TABLE radio_channels ADD COLUMN state TEXT;

DELETE FROM radio_channels;

UPDATE schema_version SET version=23;
