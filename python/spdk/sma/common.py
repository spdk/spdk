#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

import uuid


def format_volume_id(volume_id):
    """Verifies volume_id and returns it as a str

    Args:
        volume_id: either a str (in which case it's only validated) or bytes object
    """
    try:
        if type(volume_id) is bytes:
            return str(uuid.UUID(bytes=volume_id))
        elif type(volume_id) is str:
            return str(uuid.UUID(hex=volume_id))
    except ValueError:
        pass
    return None


def volume_id_to_nguid(uuid):
    return uuid.replace('-', '')
