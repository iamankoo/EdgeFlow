-- Phase 3: service registry.
--
-- A logical service (e.g. "user-service") has any number of concrete instances. The
-- registry is the persistent source of truth for service metadata; the checks below
-- repeat the application-level validation so invalid rows can never be stored.

CREATE TABLE services (
    id         BIGSERIAL   PRIMARY KEY,
    name       TEXT        NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT services_name_unique UNIQUE (name),
    CONSTRAINT services_name_format CHECK (name ~ '^[a-z0-9]([a-z0-9._-]{0,62}[a-z0-9])?$')
);

CREATE TABLE service_instances (
    id               BIGSERIAL   PRIMARY KEY,
    service_id       BIGINT      NOT NULL REFERENCES services (id) ON DELETE CASCADE,
    instance_id      TEXT        NOT NULL,
    host             TEXT        NOT NULL,
    port             INTEGER     NOT NULL,
    -- Registration state (administrative). Not the same thing as health.
    status           TEXT        NOT NULL DEFAULT 'active',
    -- Last known health, stored as metadata. Written, not probed, in Phase 3.
    health_status    TEXT        NOT NULL DEFAULT 'unknown',
    version          TEXT        NOT NULL DEFAULT '',
    weight           INTEGER     NOT NULL DEFAULT 1,
    connection_count BIGINT      NOT NULL DEFAULT 0,
    registered_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- Identity of an instance within its service. This unique index also serves the
    -- normal lookup (all instances of a service) through its service_id prefix.
    CONSTRAINT service_instances_identity UNIQUE (service_id, instance_id),
    -- The same endpoint cannot be registered twice for one service.
    CONSTRAINT service_instances_endpoint UNIQUE (service_id, host, port),

    CONSTRAINT service_instances_instance_id_format CHECK (instance_id ~ '^[A-Za-z0-9._-]{1,128}$'),
    CONSTRAINT service_instances_host_length CHECK (char_length(host) BETWEEN 1 AND 253),
    CONSTRAINT service_instances_port_range CHECK (port BETWEEN 1 AND 65535),
    CONSTRAINT service_instances_status_values CHECK (status IN ('active', 'draining', 'disabled')),
    CONSTRAINT service_instances_health_values CHECK (health_status IN ('unknown', 'healthy', 'unhealthy')),
    CONSTRAINT service_instances_version_length CHECK (char_length(version) <= 64),
    CONSTRAINT service_instances_weight_range CHECK (weight BETWEEN 0 AND 1000),
    CONSTRAINT service_instances_connections_nonnegative CHECK (connection_count >= 0)
);
