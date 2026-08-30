#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Exercise a standalone portable Class-C consumer against an independent APER peer."""

import argparse
import importlib
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading

import asn1tools
import flatbuffers

SUBSCRIPTION = bytes.fromhex("404142434445464748494a4b4c4d4e4f")
INTERFACES = {"spectrum": 0x00010001, "srs-isac": 0x00010002}


def load_type(name):
    return importlib.import_module(f"Ocudu.Dapp.ClassC.V1.{name}")


def metadata(interface, sequence, tensor_size):
    Id128 = load_type("Id128")
    RadioTime = load_type("RadioTime")
    ObservationContext = load_type("ObservationContext")
    TensorDescriptor = load_type("TensorDescriptor")
    PublicationEnvelope = load_type("PublicationEnvelope")
    PublicationKind = load_type("PublicationKind").PublicationKind
    PublicationMetadata = load_type("PublicationMetadata").PublicationMetadata
    MemoryDomain = load_type("MemoryDomain").MemoryDomain
    ElementType = load_type("ElementType").ElementType

    identity = Id128.Id128T()
    identity.low = 1
    identity.high = 2
    radio_time = RadioTime.RadioTimeT()
    radio_time.sfn = 10
    radio_time.slot = 4
    radio_time.numerology = 1
    context = ObservationContext.ObservationContextT()
    context.radioTime = radio_time
    context.cellId = 3
    context.ranNodeId = identity
    context.clockDomainId = "monotonic"
    context.observationTimeNs = 100
    tensor = TensorDescriptor.TensorDescriptorT()
    tensor.objectId = identity
    tensor.allocationGeneration = 1
    tensor.domain = MemoryDomain.Host
    tensor.backendId = 0
    tensor.elementType = ElementType.ComplexFp32
    tensor.layoutId = "reference-complex"
    tensor.layoutVersionMajor = 1
    tensor.byteLength = tensor_size
    tensor.dimensions = [1, 1, tensor_size // 8]
    tensor.byteStrides = [tensor_size, tensor_size, 8]
    tensor.deviceOrdinal = 65535
    tensor.leaseId = identity
    tensor.leaseExpiryNs = 1000
    tensor.readOnly = True

    envelope = PublicationEnvelope.PublicationEnvelopeT()
    envelope.schemaId = "org.ocudu.dapp.class-c.publication"
    envelope.schemaVersionMajor = 1
    envelope.publicationSequence = sequence
    envelope.producerInstance = identity
    envelope.context = context
    envelope.tensor = tensor
    if interface == "spectrum":
        SpectrumMetadata = load_type("SpectrumMetadata")
        body = SpectrumMetadata.SpectrumMetadataT()
        body.bandwidthHz = 20_000_000
        body.rxPorts = 1
        body.symbols = 1
        body.subcarriers = tensor_size // 8
        body.scale = 1.0
        envelope.publicationKind = PublicationKind.Spectrum
        envelope.metadataType = PublicationMetadata.SpectrumMetadata
    else:
        SrsIsacMetadata = load_type("SrsIsacMetadata")
        body = SrsIsacMetadata.SrsIsacMetadataT()
        body.srsResourceId = 2
        body.srsConfigId = 3
        body.rxPorts = 1
        body.txPorts = 1
        body.combSize = 2
        body.subcarrierSpacingHz = 30_000
        body.frequencyPoints = tensor_size // 8
        body.noiseVariance = 0.125
        body.rsrp = -8.0
        body.epre = -7.0
        envelope.publicationKind = PublicationKind.SrsIsac
        envelope.metadataType = PublicationMetadata.SrsIsacMetadata
    envelope.metadata = body
    builder = flatbuffers.Builder(1024)
    root = envelope.Pack(builder)
    builder.Finish(root, file_identifier=b"OCC3")
    return bytes(builder.Output())


def record(interface):
    counter = (0).to_bytes(8, "big")
    return {
        "subscriptionId": SUBSCRIPTION,
        "interfaceId": interface,
        "sampleEveryN": 1,
        "queueDepthChunks": 64,
        "maximumChunkBytes": 65536,
        "leaseUs": 1_000_000,
        "dropPolicy": "dropNewest",
        "leaseExpiryNs": (1).to_bytes(8, "big"),
        "queuedChunks": counter,
        "publishedObjects": counter,
        "publishedTensorBytes": counter,
        "droppedObjects": counter,
        "busyDrops": counter,
        "sampledObjects": counter,
        "active": True,
    }


def publication(interface, sequence, body, tensor):
    return {
        "subscriptionId": SUBSCRIPTION,
        "interfaceId": interface,
        "cellId": 3,
        "publicationSequence": sequence.to_bytes(8, "big"),
        "routeEpoch": (17).to_bytes(8, "big"),
        "chunkIndex": 0,
        "chunkCount": 1,
        "tensorSize": len(tensor).to_bytes(8, "big"),
        "droppedObjects": (0).to_bytes(8, "big"),
        "metadata": body,
        "tensorChunk": tensor,
    }


def serve(codec, path, interface, chunks, failures):
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(path)
    Path(path).chmod(0o600)
    listener.listen(1)
    try:
        for expected in ("dataSubscriptions", "dataPoll", "dataSubscriptions"):
            connection, _ = listener.accept()
            with connection:
                request = codec.decode("ManagementRequest", connection.recv(2 * 1024 * 1024))
                assert request["operation"][0] == expected
                if expected == "dataSubscriptions":
                    action = request["operation"][1]["action"]
                    assert action in ("create", "remove")
                    payload = (
                        "dataSubscriptions",
                        {"subscriptions": [record(interface)]},
                    )
                else:
                    assert request["operation"][1]["maximumChunks"] == 8
                    payload = (
                        "dataPublications",
                        {"chunks": chunks, "subscription": record(interface)},
                    )
                response = codec.encode(
                    "ManagementResponse",
                    {
                        "transactionId": request["header"]["transactionId"],
                        "code": "success",
                        "payload": payload,
                    },
                )
                assert connection.send(response) == len(response)
    except BaseException as error:
        failures.append(error)
    finally:
        listener.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer", required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--class-c-schema", required=True)
    parser.add_argument("--flatc", required=True)
    parser.add_argument("--interface", choices=INTERFACES, required=True)
    args = parser.parse_args()
    interface = INTERFACES[args.interface]
    codec = asn1tools.compile_files(args.schema, "per")
    tensor = b"".join(struct.pack("<ff", value, -value) for value in (1.0, 2.0, 3.0, 4.0))

    with tempfile.TemporaryDirectory(prefix="ocudu-portable-e3-") as directory:
        subprocess.run(
            [args.flatc, "--python", "--gen-object-api", "-o", directory, args.class_c_schema],
            check=True,
            timeout=10,
        )
        sys.path.insert(0, directory)
        chunks = [
            publication(interface, sequence, metadata(args.interface, sequence, len(tensor)), tensor)
            for sequence in (1, 2)
        ]
        socket_path = str(Path(directory) / "e3.sock")
        failures = []
        server = threading.Thread(target=serve, args=(codec, socket_path, interface, chunks, failures))
        server.start()
        command = [
            args.consumer,
            "--socket",
            socket_path,
            "--subscription",
            SUBSCRIPTION.hex(),
            "--maximum-objects",
            "2",
        ]
        completed = subprocess.run(command, text=True, capture_output=True, timeout=10, check=False)
        server.join(timeout=10)
        assert not server.is_alive(), "APER peer did not terminate"
        if failures:
            raise failures[0]
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
        assert completed.stdout.count("sequence=") == 2, completed.stdout


if __name__ == "__main__":
    main()
