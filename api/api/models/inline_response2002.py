# coding: utf-8

from __future__ import absolute_import
from datetime import date, datetime  # noqa: F401

from typing import List, Dict  # noqa: F401

from api.models.base_model_ import Model
from api.models.api_response import ApiResponse  # noqa: F401,E501
from api import util


class InlineResponse2002(Model):
    """NOTE: This class is auto generated by the swagger code generator program.

    Do not edit the class manually.
    """

    def __init__(self, api_response: ApiResponse=None):  # noqa: E501
        """InlineResponse2002 - a model defined in Swagger

        :param api_response: The api_response of this InlineResponse2002.  # noqa: E501
        :type api_response: ApiResponse
        """
        self.swagger_types = {
            'api_response': ApiResponse
        }

        self.attribute_map = {
            'api_response': 'ApiResponse'
        }

        self._api_response = api_response

    @classmethod
    def from_dict(cls, dikt) -> 'InlineResponse2002':
        """Returns the dict as a model

        :param dikt: A dict.
        :type: dict
        :return: The inline_response_200_2 of this InlineResponse2002.  # noqa: E501
        :rtype: InlineResponse2002
        """
        return util.deserialize_model(dikt, cls)

    @property
    def api_response(self) -> ApiResponse:
        """Gets the api_response of this InlineResponse2002.


        :return: The api_response of this InlineResponse2002.
        :rtype: ApiResponse
        """
        return self._api_response

    @api_response.setter
    def api_response(self, api_response: ApiResponse):
        """Sets the api_response of this InlineResponse2002.


        :param api_response: The api_response of this InlineResponse2002.
        :type api_response: ApiResponse
        """

        self._api_response = api_response
